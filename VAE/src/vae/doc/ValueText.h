#pragma once

#include "vae/doc/Value.h"

#include <optional>
#include <string>
#include <string_view>

namespace vae::doc {

    // The scalar half of writing a document: turning a number, a colour or a string into text and
    // back. Shared by both codecs — the JSON one and the XML one disagree about where a value goes,
    // not about how a colour is spelled — so it lives here rather than being written twice.
    namespace text {

        // Sigils, so a value can be written as itself rather than as a {"type","value"} pair. A
        // leading character settles every ambiguity the type tag was guarding: '@' a token, '='
        // a binding, '#' a colour, '&' a node, '*' an asset, and '$' escapes a literal string that
        // happens to start with one.
        inline constexpr std::string_view kSigils = "@=#$&*";

        inline bool StartsWithSigil(std::string_view s) {
            return !s.empty() && kSigils.find(s[0]) != std::string_view::npos;
        }

        // Adds the '$' that says "the next character is data, not a sigil".
        std::string EscapeLiteral(std::string_view s);
        // Removes it. A string that does not start with '$' comes back unchanged.
        std::string UnescapeLiteral(std::string_view s);

        // Shortest decimal that reads back as the SAME f32. Not the same as printing a double:
        // 0.6f promoted to double is 0.6000000238418579, which is nineteen characters of noise
        // that also fails to say what the value actually is.
        std::string Number(f32 value);
        std::optional<f32> ParseNumber(std::string_view s);

        // The double nearest to what Number() writes. For handing an f32 to a JSON library whose
        // only float type is double: the text is what a reader wants to see, and narrowing it back
        // to f32 recovers the original bits exactly.
        f64 NumberAsDouble(f32 value);

        // 8-bit hex when it survives the trip and nothing when it does not, because a colour that
        // came from anywhere but a picker (a tint computed in code, a wide-gamut value) must not be
        // quietly rounded on save.
        std::optional<std::string> ColorToHex(const Color& c);
        std::optional<Color> ColorFromHex(std::string_view hex);

    }

}
