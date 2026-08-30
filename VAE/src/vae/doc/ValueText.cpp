#include "vaepch.h"
#include "vae/doc/ValueText.h"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>

namespace vae::doc::text {

    std::string EscapeLiteral(std::string_view s) {
        if (StartsWithSigil(s)) return "$" + std::string(s);
        return std::string(s);
    }

    std::string UnescapeLiteral(std::string_view s) {
        if (!s.empty() && s[0] == '$') return std::string(s.substr(1));
        return std::string(s);
    }

    std::string Number(f32 value) {
        // to_chars on the f32 itself, which is what makes this shortest-round-trip rather than
        // shortest-for-a-double. No precision argument: that asks for a fixed number of digits and
        // gets 0.600000 back.
        std::array<char, 32> buf{};
        const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
        if (ec != std::errc{}) return "0";
        return std::string(buf.data(), ptr);
    }

    std::optional<f32> ParseNumber(std::string_view s) {
        // Leading whitespace is not from_chars' problem, and a document should not have any; a
        // hand-edited one might, so it is trimmed rather than rejected.
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        if (s.empty()) return std::nullopt;

        f32 out = 0.0f;
        const char* first = s.data();
        const char* last = s.data() + s.size();
        auto [ptr, ec] = std::from_chars(first, last, out);
        if (ec != std::errc{}) return std::nullopt;
        // A partial parse is a different value, not this one: "12px" is not 12.
        while (ptr != last && (*ptr == ' ' || *ptr == '\t')) ++ptr;
        if (ptr != last) return std::nullopt;
        return out;
    }

    f64 NumberAsDouble(f32 value) {
        if (!std::isfinite(value)) return static_cast<f64>(value);
        const std::string s = Number(value);
        f64 out = 0.0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        if (ec != std::errc{} || ptr != s.data() + s.size()) return static_cast<f64>(value);
        return out;
    }

    std::optional<std::string> ColorToHex(const Color& c) {
        char buf[10];
        u8 bytes[4];
        for (int i = 0; i < 4; ++i) {
            const f32 v = c[i];
            if (!(v >= 0.0f && v <= 1.0f)) return std::nullopt;
            const f32 q = std::round(v * 255.0f);
            if (q / 255.0f != v) return std::nullopt;
            bytes[i] = static_cast<u8>(q);
        }
        const int n = bytes[3] == 255
            ? std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", bytes[0], bytes[1], bytes[2])
            : std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2],
                            bytes[3]);
        return std::string(buf, static_cast<std::size_t>(n));
    }

    std::optional<Color> ColorFromHex(std::string_view hex) {
        if (hex.empty() || hex.front() != '#') return std::nullopt;
        hex.remove_prefix(1);
        if (hex.size() != 6 && hex.size() != 8) return std::nullopt;
        u32 bytes[4] = { 0, 0, 0, 255 };
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            const char* first = hex.data() + i;
            auto [ptr, ec] = std::from_chars(first, first + 2, bytes[i / 2], 16);
            if (ec != std::errc{} || ptr != first + 2) return std::nullopt;
        }
        return Color{ bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f };
    }

    std::string Vec2Text(const Vec2& v) { return Number(v.x) + " " + Number(v.y); }

    std::optional<Vec2> Vec2FromText(std::string_view s) {
        f32 parts[2]{};
        std::size_t found = 0;
        std::size_t at = 0;
        while (at < s.size() && found < 2) {
            while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
            const std::size_t start = at;
            while (at < s.size() && !std::isspace(static_cast<unsigned char>(s[at]))) ++at;
            if (at == start) break;
            const auto value = ParseNumber(s.substr(start, at - start));
            if (!value) return std::nullopt;
            parts[found++] = *value;
        }
        while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
        if (found != 2 || at != s.size()) return std::nullopt;
        return Vec2{ parts[0], parts[1] };
    }

    std::string EscapeAttr(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '&':  out += "&amp;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                case '"':  out += "&quot;"; break;
                case '\n': out += "&#10;";  break;
                case '\t': out += "&#9;";   break;
                case '\r': out += "&#13;";  break;
                default:   out += c;        break;
            }
        }
        return out;
    }

    std::string ValueAsText(const Value& value) {
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)             return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, f32>)         return Number(v);
            else if constexpr (std::is_same_v<T, Vec2>)        return Vec2Text(v);
            else if constexpr (std::is_same_v<T, Color>)       return ColorToHex(v).value_or("");
            else if constexpr (std::is_same_v<T, std::string>) return v;
            else if constexpr (std::is_same_v<T, TokenRef>)    return "@" + v.name;
            else if constexpr (std::is_same_v<T, Binding>)     return "=" + v.expression;
            else return std::string();
        }, value);
    }

    Value ValueFromText(std::string_view text, ValueType type) {
        switch (type) {
            case ValueType::Bool:
                // The same words a row cell means by false, so one boundary carrying a value as
                // text does not disagree with the other about what "off" is.
                return !(text.empty() || text == "0" || text == "no" || text == "off"
                         || text == "false");
            case ValueType::Number: return ParseNumber(text).value_or(0.0f);
            case ValueType::Colour: {
                if (const auto colour = ColorFromHex(text)) return *colour;
                // Not hex, so it is the name of a colour: a token, which resolves against the
                // theme the way every other colour in the document does.
                return TokenRef{ std::string(text) };
            }
            default: break;
        }
        return std::string(text);
    }

}
