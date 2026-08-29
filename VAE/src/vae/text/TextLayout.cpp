#include "vaepch.h"
#include "vae/text/TextLayout.h"

#include "vae/base/Utf8.h"
#include "vae/text/FontDB.h"

#include <hb.h>

#include <cmath>

namespace vae::text {

    namespace {
        // Characters that mean the picture, not the letter. Unicode calls this emoji presentation,
        // and the plane-1 pictographs are the block that has it by default — which is the case that
        // matters, because a text face with some outline for U+1F600 must not beat the font that
        // has the actual emoji. Below this the symbol blocks default to *text* presentation, so
        // leaving them to the text face is right. A U+FE0F on a character that would otherwise be
        // text is not honoured: that is a property of the cluster, not of one codepoint.
        bool WantsColour(u32 codepoint) {
            return codepoint >= 0x1F300 && codepoint <= 0x1FAFF;
        }
    }

    const Ref<Font>& TextStyle::FaceFor(u32 codepoint) const {
        if (WantsColour(codepoint)) {
            // ColourCovers, not Colour: an sbix or COLR face draws most of its glyphs as plain
            // outlines, so "this face can do colour" is not "this face does colour for *this*".
            if (font && font->ColourCovers(codepoint)) return font;
            for (const auto& fallback : fallbacks)
                if (fallback && fallback->ColourCovers(codepoint)) return fallback;
            if (db) {
                const Ref<Font>& colour = db->FaceCovering(codepoint, weight, slant, true);
                if (colour) return colour;
            }
        }

        if (font && font->HasGlyph(codepoint)) return font;
        for (const auto& fallback : fallbacks)
            if (fallback && fallback->HasGlyph(codepoint)) return fallback;
        if (db) {
            const Ref<Font>& covering = db->FaceCovering(codepoint, weight, slant);
            if (covering) return covering;
        }
        return font;
    }

    namespace {

        bool IsBreakSpace(u32 cp) { return cp == ' ' || cp == '\t'; }

        // Where a line may be broken *after*. Deliberately not a full UAX #14 implementation;
        // spaces and the common CJK ranges cover UI text, and anything more needs a line-breaking
        // library on top of the shaping one.
        bool AllowsBreakAfter(u32 cp) {
            if (IsBreakSpace(cp) || cp == '-') return true;
            if (cp >= 0x3040 && cp <= 0x30FF) return true;    // kana
            if (cp >= 0x4E00 && cp <= 0x9FFF) return true;    // CJK ideographs
            return false;
        }

        hb_script_t ScriptOf(u32 codepoint) {
            return hb_unicode_script(hb_unicode_funcs_get_default(), codepoint);
        }

        // Common (spaces, digits, most punctuation) and inherited (combining marks) carry no script
        // of their own: they belong to whatever they are sitting in, so they never start a run.
        bool ScriptIsNeutral(hb_script_t script) {
            return script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED
                || script == HB_SCRIPT_UNKNOWN;
        }

        bool ScriptIsRtl(hb_script_t script) {
            return HB_DIRECTION_IS_BACKWARD(hb_script_get_horizontal_direction(script));
        }

        // One shaped glyph, still in logical order.
        struct Shaped {
            u32   glyph = 0;
            u32   codepoint = 0;
            Font* face = nullptr;
            f32   advance = 0.0f;
            Vec2  offset{ 0.0f, 0.0f };
            std::size_t byteOffset = 0;
            bool  cluster = true;
            bool  rtl = false;
            u32   run = 0;
        };

        // Shaping wants a buffer, and a buffer carries HarfBuzz's scratch allocations, so one is
        // kept and reset rather than created per run. Thread-local because HarfBuzz is compiled
        // with HB_NO_MT: nothing lays out text off the main thread, and the atomics that would
        // make it safe to are pure overhead here.
        struct ShapingBuffer {
            hb_buffer_t* buffer = hb_buffer_create();
            ~ShapingBuffer() { hb_buffer_destroy(buffer); }
        };

        // A stretch of one face, one script and one direction — the unit HarfBuzz shapes.
        struct Run {
            std::size_t begin = 0, end = 0;
            Font*       face = nullptr;
            hb_script_t script = HB_SCRIPT_COMMON;
            bool        rtl = false;
        };

        struct Character {
            std::size_t at = 0, next = 0;
            u32         codepoint = 0;
            hb_script_t script = HB_SCRIPT_COMMON;
            bool        neutral = true;
            bool        rtl = false;
        };

        // A space between an Arabic word and an English one belongs to whichever of them the
        // sentence is written in — put it in the wrong one and it is drawn on the wrong side of
        // the word, which reads as a missing space. Rules N1 and N2 of UAX #9 say how to decide:
        // a stretch of neutral characters between two strong ones going the same way joins them,
        // and otherwise takes the direction of the paragraph.
        void ResolveNeutrals(std::vector<Character>& characters, bool paragraphRtl) {
            for (std::size_t i = 0; i < characters.size(); ) {
                if (!characters[i].neutral) { ++i; continue; }

                std::size_t end = i;
                while (end < characters.size() && characters[end].neutral) ++end;

                const bool beforeRtl = i > 0 ? characters[i - 1].rtl : paragraphRtl;
                const bool afterRtl  = end < characters.size() ? characters[end].rtl : paragraphRtl;
                const bool resolved  = beforeRtl == afterRtl ? beforeRtl : paragraphRtl;

                // The script comes from whichever neighbour it ended up agreeing with, so the
                // neutral shapes as part of that run instead of splitting it in two.
                hb_script_t script = resolved == paragraphRtl && paragraphRtl
                                   ? HB_SCRIPT_ARABIC : HB_SCRIPT_LATIN;
                if (i > 0 && characters[i - 1].rtl == resolved)            script = characters[i - 1].script;
                else if (end < characters.size() && characters[end].rtl == resolved) script = characters[end].script;

                for (std::size_t j = i; j < end; ++j) {
                    characters[j].rtl = resolved;
                    characters[j].script = script;
                }
                i = end;
            }
        }

        std::vector<Run> SegmentRuns(std::string_view text, std::size_t begin, std::size_t end,
                                     const TextStyle& style, bool paragraphRtl) {
            std::vector<Character> characters;
            std::size_t index = begin;
            while (index < end) {
                Character character;
                character.at = index;
                character.codepoint = Utf8Next(text, index);
                if (character.codepoint == 0) break;
                character.next = index;
                character.script = ScriptOf(character.codepoint);
                character.neutral = ScriptIsNeutral(character.script);
                character.rtl = character.neutral ? paragraphRtl : ScriptIsRtl(character.script);
                characters.push_back(character);
            }
            ResolveNeutrals(characters, paragraphRtl);

            std::vector<Run> runs;
            for (const Character& character : characters) {
                Font* face = style.FaceFor(character.codepoint).get();

                if (!runs.empty()) {
                    Run& current = runs.back();
                    // A neutral stays on the face of the run it joined as long as that face can
                    // draw it: splitting a sentence at every space would cost the shaper the
                    // context either side of one.
                    const bool sameFace = character.neutral && current.face
                                       && current.face->HasGlyph(character.codepoint)
                                        ? true : (face == current.face);
                    if (sameFace && character.script == current.script
                                 && character.rtl == current.rtl) {
                        current.end = character.next;
                        continue;
                    }
                }

                Run run;
                run.begin  = character.at;
                run.end    = character.next;
                run.face   = face;
                run.script = character.script;
                run.rtl    = character.rtl;
                runs.push_back(run);
            }
            return runs;
        }

        void ShapeRun(std::string_view text, const Run& run, const TextStyle& style, u32 runIndex,
                      std::vector<Shaped>& out) {
            if (!run.face) return;
            auto* font = static_cast<hb_font_t*>(run.face->ShaperFont(style.size));
            if (!font) return;

            static thread_local ShapingBuffer shared;
            hb_buffer_t* buffer = shared.buffer;
            hb_buffer_reset(buffer);
            // The whole string is added, with only [begin,end) marked as the item to shape, so the
            // shaper can see the characters on either side — Arabic joining depends on them.
            hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()),
                               static_cast<unsigned>(run.begin),
                               static_cast<int>(run.end - run.begin));
            hb_buffer_set_direction(buffer, run.rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            hb_buffer_set_script(buffer, run.script);
            hb_buffer_set_language(buffer, hb_language_get_default());
            hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

            // Kerning is a font feature, not something applied afterwards: turning it off means
            // asking the shaper not to run `kern`, which also covers the GPOS form of it. Kerning
            // on is the font's own default, so that case asks for no features at all — which is
            // the request HarfBuzz caches a shaping plan for.
            const hb_feature_t off{ HB_TAG('k','e','r','n'), 0, 0, static_cast<unsigned>(-1) };
            hb_shape(font, buffer, style.kerning ? nullptr : &off, style.kerning ? 0u : 1u);

            unsigned count = 0;
            const hb_glyph_info_t*     infos     = hb_buffer_get_glyph_infos(buffer, &count);
            const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);

            constexpr f32 kFromFixed = 1.0f / 64.0f;
            const std::size_t begin = out.size();
            for (unsigned i = 0; i < count; ++i) {
                Shaped shaped;
                shaped.glyph      = infos[i].codepoint;      // post-shaping this is a glyph index
                shaped.face       = run.face;
                shaped.byteOffset = infos[i].cluster;
                shaped.rtl        = run.rtl;
                shaped.run        = runIndex;

                std::size_t at = shaped.byteOffset;
                shaped.codepoint = at < text.size() ? Utf8Next(text, at) : 0;

                shaped.advance = static_cast<f32>(positions[i].x_advance) * kFromFixed;
                // y grows downward here and upward in HarfBuzz, so the offset's sign flips.
                shaped.offset  = { static_cast<f32>(positions[i].x_offset) * kFromFixed,
                                  -static_cast<f32>(positions[i].y_offset) * kFromFixed };
                out.push_back(shaped);
            }


            // HarfBuzz hands back an RTL run already in visual order. Line breaking has to happen
            // in *logical* order — where a line ends is a property of the text, not of which side
            // of the screen it starts on — so it is turned back here and reversed again, per run,
            // once the lines are known.
            if (run.rtl) std::reverse(out.begin() + static_cast<std::ptrdiff_t>(begin), out.end());

            std::size_t previousCluster = SIZE_MAX;
            for (std::size_t i = begin; i < out.size(); ++i) {
                out[i].cluster = out[i].byteOffset != previousCluster;
                previousCluster = out[i].byteOffset;
                // Letter spacing is between clusters, not between a letter and the mark on it.
                if (out[i].cluster) out[i].advance += style.letterSpacing;
            }
        }

        bool ParagraphIsRtl(std::string_view text, std::size_t begin, std::size_t end) {
            std::size_t index = begin;
            while (index < end) {
                const u32 codepoint = Utf8Next(text, index);
                if (codepoint == 0) break;
                const hb_script_t script = ScriptOf(codepoint);
                if (ScriptIsNeutral(script)) continue;
                return ScriptIsRtl(script);
            }
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

        // Shape first, break second. Every glyph of the whole string, in logical order, with the
        // run it came from — line breaking then only chooses where to cut this array.
        std::vector<Shaped> shaped;
        std::vector<u8>     breakBefore(utf8.size() + 1, 0);
        std::vector<bool>   paragraphRtlOf;      // per run: was its paragraph right-to-left?
        std::vector<std::size_t> paragraphEnds;  // shaped-index just past each paragraph

        {
            std::size_t paragraphBegin = 0;
            while (paragraphBegin <= utf8.size()) {
                std::size_t paragraphEnd = utf8.find('\n', paragraphBegin);
                const bool last = paragraphEnd == std::string_view::npos;
                if (last) paragraphEnd = utf8.size();

                // Carriage returns are not glyphs and not breaks; a CRLF file must not draw a box.
                std::size_t trimmedEnd = paragraphEnd;
                while (trimmedEnd > paragraphBegin && utf8[trimmedEnd - 1] == '\r') --trimmedEnd;

                const bool rtl = ParagraphIsRtl(utf8, paragraphBegin, trimmedEnd);
                for (const Run& run : SegmentRuns(utf8, paragraphBegin, trimmedEnd, style, rtl)) {
                    const auto index = static_cast<u32>(paragraphRtlOf.size());
                    paragraphRtlOf.push_back(rtl);
                    ShapeRun(utf8, run, style, index, shaped);
                }

                // Break opportunities come from the source text, not from the shaped glyphs: a
                // ligature spanning a hyphen still breaks where the hyphen is.
                std::size_t index = paragraphBegin;
                while (index < trimmedEnd) {
                    const u32 codepoint = Utf8Next(utf8, index);
                    if (codepoint == 0) break;
                    if (AllowsBreakAfter(codepoint)) breakBefore[index] = 1;
                }

                paragraphEnds.push_back(shaped.size());
                if (last) break;
                paragraphBegin = paragraphEnd + 1;
            }
        }

        u32 lineIndex = 0;
        std::size_t lineStart = 0;

        // One line, from a logical range of shaped glyphs: trim, reorder for direction, place.
        auto EmitLine = [&](std::size_t first, std::size_t last, bool paragraphRtl) {
            f32 width = 0.0f;
            for (std::size_t i = first; i < last; ++i) width += shaped[i].advance;
            // Trailing whitespace must not count: a wrapped line ending in a space would otherwise
            // report a width wider than its visible ink and misalign centred text.
            for (std::size_t i = last; i > first; --i) {
                if (!IsBreakSpace(shaped[i - 1].codepoint)) break;
                width -= shaped[i - 1].advance;
            }
            width = std::max(width, 0.0f);

            // Level 2 of the bidi algorithm, over runs rather than over resolved embedding levels:
            // an RTL run reads right to left inside itself, and in an RTL paragraph the runs
            // themselves are laid out right to left as well.
            std::vector<Shaped> visual;
            visual.reserve(last - first);
            {
                std::vector<std::pair<std::size_t, std::size_t>> groups;
                for (std::size_t i = first; i < last; ) {
                    std::size_t end = i + 1;
                    while (end < last && shaped[end].run == shaped[i].run) ++end;
                    groups.emplace_back(i, end);
                    i = end;
                }
                if (paragraphRtl) std::reverse(groups.begin(), groups.end());

                for (const auto& [groupBegin, groupEnd] : groups) {
                    const auto begin = shaped.begin() + static_cast<std::ptrdiff_t>(groupBegin);
                    const auto end   = shaped.begin() + static_cast<std::ptrdiff_t>(groupEnd);
                    if (shaped[groupBegin].rtl) visual.insert(visual.end(),
                                                              std::make_reverse_iterator(end),
                                                              std::make_reverse_iterator(begin));
                    else                        visual.insert(visual.end(), begin, end);
                }
            }

            TextLine line;
            line.firstGlyph = static_cast<u32>(result.glyphs.size());
            line.glyphCount = static_cast<u32>(visual.size());
            line.width = width;
            line.baselineY = static_cast<f32>(lineIndex) * lineHeight - metrics.ascent;

            f32 penX = 0.0f;
            for (const Shaped& source : visual) {
                PositionedGlyph glyph;
                glyph.glyph      = source.glyph;
                glyph.codepoint  = source.codepoint;
                glyph.face       = source.face;
                glyph.pen        = { penX, 0.0f };   // baseline filled in once the line is placed
                glyph.offset     = source.offset;
                glyph.advance    = source.advance;
                glyph.line       = lineIndex;
                glyph.byteOffset = source.byteOffset;
                glyph.cluster    = source.cluster;
                result.glyphs.push_back(glyph);
                penX += source.advance;
            }

            result.lines.push_back(line);
            ++lineIndex;
        };

        for (std::size_t paragraphEnd : paragraphEnds) {
            const bool rtl = lineStart < paragraphEnd ? paragraphRtlOf[shaped[lineStart].run]
                                                      : false;

            f32 penX = 0.0f;
            std::size_t lastBreak = SIZE_MAX;

            for (std::size_t i = lineStart; i < paragraphEnd; ++i) {
                const Shaped& glyph = shaped[i];
                // Only a cluster boundary is a legal cut: breaking inside one would split a
                // ligature or orphan a combining mark from the letter it sits on.
                if (glyph.cluster && breakBefore[glyph.byteOffset] && i > lineStart) lastBreak = i;

                // A space that runs past the edge does not wrap: it is trimmed off the line's width
                // anyway, and breaking on it would put the space alone at the head of the next line.
                const bool overflows = bounded && penX + glyph.advance > maxWidth
                                    && i > lineStart && !IsBreakSpace(glyph.codepoint);
                if (overflows) {
                    const std::size_t cut = wrap == WrapMode::Word && lastBreak != SIZE_MAX
                                          ? lastBreak
                                          : (glyph.cluster ? i : SIZE_MAX);
                    // Char wrap, or a single word longer than the line, breaks where we are —
                    // unless we are mid-cluster, where there is no legal place to cut at all.
                    if (cut != SIZE_MAX) {
                        EmitLine(lineStart, cut, rtl);
                        lineStart = cut;
                        lastBreak = SIZE_MAX;
                        penX = 0.0f;
                        for (std::size_t j = lineStart; j < i; ++j) penX += shaped[j].advance;
                    }
                }
                penX += glyph.advance;
            }

            EmitLine(lineStart, paragraphEnd, rtl);
            lineStart = paragraphEnd;
        }

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
            if (!glyph.cluster) continue;     // a caret cannot sit inside a cluster
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
