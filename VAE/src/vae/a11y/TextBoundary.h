#pragma once

#include "vae/base/Base.h"

#include <string>
#include <string_view>
#include <vector>

namespace vae::a11y {

    // How a screen reader reads a piece of text: not all at once, but a character, a word, a line
    // or a sentence at a time, walking forwards and backwards through it with the arrow keys.
    // Answering "what is the word at offset 12" is therefore not a detail of the bus protocol —
    // it is the whole of how the text gets read out — which is why it lives here, in the library
    // the headless suite can reach, rather than inside the D-Bus bridge where nothing could check
    // it without a screen reader attached.
    //
    // Offsets are in **characters**, because that is what AT-SPI counts. A caret in VAE is a byte
    // index, and in a field holding anything but ASCII the two are different numbers.
    class Characters {
    public:
        explicit Characters(std::string_view text);

        i32 Count() const { return static_cast<i32>(m_Code.size()); }
        i32 Clamp(i32 offset) const;
        // The codepoint at an offset, or 0 past either end — so a caller walking off the end reads
        // something that is not a character rather than something that is not there.
        u32 CodeAt(i32 offset) const;
        std::size_t ByteAt(i32 offset) const;
        std::string_view Slice(i32 from, i32 to) const;
        std::string_view Text() const { return m_Text; }

    private:
        std::string_view m_Text;             // borrowed: the caller owns the string
        std::vector<std::size_t> m_ByteAt;   // Count() + 1 entries, the last one past the end
        std::vector<u32> m_Code;
    };

    struct Range {
        i32 start = 0;
        i32 end = 0;
        bool operator==(const Range&) const = default;
    };

    // AtspiTextGranularity and AtspiTextBoundaryType, with AT-SPI's own numbering. Two enumerations
    // for nearly the same question, because the second is the old API and screen readers still call
    // it. They differ in what the range *includes*: a "word start" range runs to the start of the
    // next word, so the spaces belong to the word before them, while WordRange stops at the word.
    enum class Granularity : u32 { Char = 0, Word = 1, Sentence = 2, Line = 3, Paragraph = 4 };
    enum class Boundary : u32 {
        Char = 0, WordStart = 1, WordEnd = 2, SentenceStart = 3, SentenceEnd = 4,
        LineStart = 5, LineEnd = 6,
    };

    Range CharacterRange(const Characters& text, i32 offset);
    Range WordRange(const Characters& text, i32 offset);
    Range LineRange(const Characters& text, i32 offset);
    // The sentence containing an offset. Terminated by `.`, `!`, `?` or `…` — plus any closing
    // quote or bracket that follows — and by a line break, which ends a sentence whether or not
    // anybody punctuated it. A field with no terminator in it is one sentence, which is the
    // common case and the one the old answer got right by accident.
    Range SentenceRange(const Characters& text, i32 offset);

    Range RangeFor(const Characters& text, i32 offset, Granularity granularity);
    Range RangeFor(const Characters& text, i32 offset, Boundary boundary);

}
