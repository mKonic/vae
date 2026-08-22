#pragma once

#include "vae/base/Base.h"

#include <string>
#include <string_view>

namespace vae {

    // Minimal UTF-8 decoding. Malformed bytes decode to U+FFFD and advance by one, so a bad byte
    // can never desynchronise the rest of a string or spin the caller's loop forever.
    constexpr u32 kReplacementChar = 0xFFFD;

    inline u32 Utf8Next(std::string_view text, std::size_t& index) {
        if (index >= text.size()) return 0;

        const auto byte = static_cast<u8>(text[index]);
        auto Continuation = [&](std::size_t at) -> bool {
            return at < text.size() && (static_cast<u8>(text[at]) & 0xC0) == 0x80;
        };
        auto Tail = [&](std::size_t at) -> u32 { return static_cast<u8>(text[at]) & 0x3Fu; };

        if (byte < 0x80) { ++index; return byte; }

        if ((byte & 0xE0) == 0xC0 && Continuation(index + 1)) {
            const u32 cp = ((byte & 0x1Fu) << 6) | Tail(index + 1);
            index += 2;
            return cp;
        }
        if ((byte & 0xF0) == 0xE0 && Continuation(index + 1) && Continuation(index + 2)) {
            const u32 cp = ((byte & 0x0Fu) << 12) | (Tail(index + 1) << 6) | Tail(index + 2);
            index += 3;
            return cp;
        }
        if ((byte & 0xF8) == 0xF0 && Continuation(index + 1) && Continuation(index + 2)
                                  && Continuation(index + 3)) {
            const u32 cp = ((byte & 0x07u) << 18) | (Tail(index + 1) << 12)
                         | (Tail(index + 2) << 6) | Tail(index + 3);
            index += 4;
            return cp;
        }

        ++index;
        return kReplacementChar;
    }

    inline std::size_t Utf8Length(std::string_view text) {
        std::size_t count = 0, index = 0;
        while (index < text.size()) { Utf8Next(text, index); ++count; }
        return count;
    }

    inline void Utf8Append(std::string& out, u32 codepoint) {
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

}
