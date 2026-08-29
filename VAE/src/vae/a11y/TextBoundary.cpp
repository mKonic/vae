#include "vaepch.h"
#include "vae/a11y/TextBoundary.h"

#include "vae/base/Utf8.h"

#include <algorithm>

namespace vae::a11y {

    namespace {

        bool IsSpace(u32 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

        // What ends a sentence, before anything that closes around it.
        bool IsTerminator(u32 c) {
            return c == '.' || c == '!' || c == '?' || c == 0x2026   // HORIZONTAL ELLIPSIS
                || c == 0x061F                                       // ARABIC QUESTION MARK
                || c == 0x3002;                                      // IDEOGRAPHIC FULL STOP
        }

        // A quote or bracket that belongs to the sentence it closes: `He said "go."` ends after
        // the quotation mark, not before it.
        bool IsCloser(u32 c) {
            return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}'
                || c == 0x201D || c == 0x2019 || c == 0x00BB;        // ” ’ »
        }

        // A capital, a digit, or something that opens a quotation — what the next sentence starts
        // with. This is the whole of the abbreviation heuristic: "e.g. this" does not split
        // because `t` is lowercase, and "End. Next" does because `N` is not. It gets "Dr. Smith"
        // wrong, and every splitter without a dictionary of abbreviations does; the alternative —
        // treating every period as a sentence end — is wrong far more often.
        bool StartsASentence(u32 c) {
            if (c == 0) return true;                                 // the end of the text
            if (c >= 'a' && c <= 'z') return false;
            // Anything outside ASCII is left alone: a script with no case at all (CJK, Arabic,
            // Devanagari) must not be told that its next sentence looks lowercase.
            return true;
        }

    }

    Characters::Characters(std::string_view text) : m_Text(text) {
        m_ByteAt.reserve(text.size() + 1);
        m_Code.reserve(text.size());
        std::size_t at = 0;
        while (at < text.size()) {
            m_ByteAt.push_back(at);
            m_Code.push_back(Utf8Next(text, at));
        }
        m_ByteAt.push_back(text.size());
    }

    i32 Characters::Clamp(i32 offset) const { return std::clamp(offset, 0, Count()); }

    u32 Characters::CodeAt(i32 offset) const {
        if (offset < 0 || offset >= Count()) return 0;
        return m_Code[static_cast<std::size_t>(offset)];
    }

    std::size_t Characters::ByteAt(i32 offset) const {
        return m_ByteAt[static_cast<std::size_t>(Clamp(offset))];
    }

    std::string_view Characters::Slice(i32 from, i32 to) const {
        from = Clamp(from);
        to   = Clamp(to);
        if (to <= from) return {};
        return m_Text.substr(ByteAt(from), ByteAt(to) - ByteAt(from));
    }

    Range CharacterRange(const Characters& text, i32 offset) {
        return { text.Clamp(offset), text.Clamp(offset + 1) };
    }

    Range WordRange(const Characters& text, i32 offset) {
        const i32 count = text.Count();
        if (count == 0) return {};
        offset = std::clamp(offset, 0, count - 1);
        // The run of spaces between two words is not a word, and saying it is one makes a reader
        // announce nothing between every pair of them.
        if (IsSpace(text.CodeAt(offset))) return { offset, offset };
        Range range{ offset, offset };
        while (range.start > 0 && !IsSpace(text.CodeAt(range.start - 1))) --range.start;
        while (range.end < count && !IsSpace(text.CodeAt(range.end))) ++range.end;
        return range;
    }

    Range LineRange(const Characters& text, i32 offset) {
        const i32 count = text.Count();
        offset = std::clamp(offset, 0, count);
        Range range{ offset, offset };
        while (range.start > 0 && text.CodeAt(range.start - 1) != '\n') --range.start;
        while (range.end < count && text.CodeAt(range.end) != '\n') ++range.end;
        return range;
    }

    Range SentenceRange(const Characters& text, i32 offset) {
        const i32 count = text.Count();
        if (count == 0) return {};
        offset = std::clamp(offset, 0, count);

        // Where the sentence containing `at` ends: past the terminator and anything closing after
        // it, but not past the space that follows — the space belongs to the gap between them.
        const auto EndFrom = [&](i32 at) {
            for (i32 i = at; i < count; ++i) {
                if (text.CodeAt(i) == '\n') return i;                // a line break ends one too
                if (!IsTerminator(text.CodeAt(i))) continue;
                i32 end = i + 1;
                while (end < count && (IsTerminator(text.CodeAt(end)) || IsCloser(text.CodeAt(end))))
                    ++end;
                // Only if what comes next looks like the start of another sentence. Otherwise this
                // was a decimal point or an abbreviation and the sentence carries on through it.
                i32 next = end;
                while (next < count && IsSpace(text.CodeAt(next))) ++next;
                if (next == end && next < count) continue;           // "1.5" — no gap at all
                if (!StartsASentence(text.CodeAt(next))) continue;
                return end;
            }
            return count;
        };

        // Backwards: the previous sentence's end is this one's start. Walked from the beginning
        // rather than scanned backwards, because whether a period ends a sentence depends on what
        // follows it, and reading that backwards means deciding the same question twice.
        i32 start = 0;
        for (;;) {
            const i32 end = EndFrom(start);
            i32 next = end;
            while (next < count && IsSpace(text.CodeAt(next))) ++next;
            // The offset is inside this sentence, or in the gap after it — a caret sitting on the
            // space between two sentences belongs to the one it was typed at the end of.
            if (offset < next || next >= count || end >= count) return { start, end };
            if (next <= start) return { start, count };              // no progress: one sentence
            start = next;
        }
    }

    Range RangeFor(const Characters& text, i32 offset, Granularity granularity) {
        switch (granularity) {
            case Granularity::Word:      return WordRange(text, offset);
            case Granularity::Line:
            // A paragraph is a line: VAE's text is broken by newlines and nothing else, so the two
            // questions have the same answer and inventing a difference would be inventing
            // structure the document does not have.
            case Granularity::Paragraph: return LineRange(text, offset);
            case Granularity::Sentence:  return SentenceRange(text, offset);
            case Granularity::Char:
            default:                     return CharacterRange(text, offset);
        }
    }

    Range RangeFor(const Characters& text, i32 offset, Boundary boundary) {
        const i32 count = text.Count();
        switch (boundary) {
            case Boundary::WordStart: {
                Range range = WordRange(text, offset);
                while (range.end < count && IsSpace(text.CodeAt(range.end))) ++range.end;
                return range;
            }
            case Boundary::WordEnd: {
                Range range = WordRange(text, offset);
                while (range.start > 0 && IsSpace(text.CodeAt(range.start - 1))) --range.start;
                return range;
            }
            case Boundary::LineStart: {
                Range range = LineRange(text, offset);
                if (range.end < count) ++range.end;                  // the newline ends the line
                return range;
            }
            case Boundary::LineEnd: {
                Range range = LineRange(text, offset);
                if (range.start > 0) --range.start;
                return range;
            }
            case Boundary::SentenceStart: {
                Range range = SentenceRange(text, offset);
                while (range.end < count && IsSpace(text.CodeAt(range.end))) ++range.end;
                return range;
            }
            case Boundary::SentenceEnd: {
                Range range = SentenceRange(text, offset);
                while (range.start > 0 && IsSpace(text.CodeAt(range.start - 1))) --range.start;
                return range;
            }
            case Boundary::Char:
            default:                     return CharacterRange(text, offset);
        }
    }

}
