#include "vaepch.h"
#include "vae/doc/ValueText.h"

#include <array>
#include <charconv>
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

}
