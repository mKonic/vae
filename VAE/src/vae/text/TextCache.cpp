#include "vaepch.h"
#include "vae/text/TextCache.h"

#include <deque>

namespace vae::text {

    namespace {

        // Everything that changes the shaped result and is not the string itself. Compared field by
        // field on a hash hit rather than trusted: a 64-bit collision would silently draw one
        // label's glyphs at another label's advances, which is the kind of bug that gets blamed on
        // the font.
        struct StyleKey {
            const Font* font = nullptr;
            f32 size = 0.0f;
            f32 lineHeight = 0.0f;
            f32 letterSpacing = 0.0f;
            f32 maxWidth = 0.0f;
            u32 fallbacks = 0;              // the chain's identity, hashed
            WrapMode wrap = WrapMode::Word;
            TextAlign align = TextAlign::Left;
            bool kerning = true;

            bool operator==(const StyleKey&) const = default;
        };

        StyleKey KeyOf(const TextStyle& style, f32 maxWidth, WrapMode wrap, TextAlign align) {
            StyleKey key;
            key.font = style.font.get();
            key.size = style.size;
            key.lineHeight = style.lineHeight;
            key.letterSpacing = style.letterSpacing;
            // Only the width the text was wrapped into matters, and only when it wraps: an
            // unwrapped label measured at 300 and at 900 comes out identical, and keying on the
            // number would miss every time a panel resized.
            key.maxWidth = wrap == WrapMode::None ? 0.0f : maxWidth;
            key.wrap = wrap;
            key.align = align;
            key.kerning = style.kerning;
            // The fallback chain resolves codepoints the main face lacks, so two styles with the
            // same face and different chains can shape differently.
            u32 chain = 2166136261u;
            for (const Ref<Font>& face : style.fallbacks) {
                const auto bits = reinterpret_cast<std::uintptr_t>(face.get());
                chain = (chain ^ static_cast<u32>(bits)) * 16777619u;
                chain = (chain ^ static_cast<u32>(bits >> 32)) * 16777619u;
            }
            key.fallbacks = chain;
            return key;
        }

        u64 HashOf(std::string_view text, const StyleKey& key) {
            u64 h = 1469598103934665603ull;
            const auto mix = [&h](u64 value) {
                h ^= value;
                h *= 1099511628211ull;
            };
            for (const char c : text) mix(static_cast<u8>(c));
            mix(reinterpret_cast<std::uintptr_t>(key.font));
            const auto bits = [](f32 v) { u32 out; std::memcpy(&out, &v, sizeof out); return u64(out); };
            mix(bits(key.size));
            mix(bits(key.lineHeight));
            mix(bits(key.letterSpacing));
            mix(bits(key.maxWidth));
            mix(key.fallbacks);
            mix(static_cast<u64>(key.wrap) | (static_cast<u64>(key.align) << 8)
                | (static_cast<u64>(key.kerning) << 16));
            return h;
        }

        struct Entry {
            std::string text;
            StyleKey key;
            TextLayoutResult result;
            u64 touched = 0;
        };

        // Bounded so a headless run that never sweeps — the test suite, a --convert — cannot grow
        // without limit. A screen's worth of distinct labels is in the hundreds; this is room for
        // a document far larger than anything anyone has drawn.
        constexpr std::size_t kMaxEntries = 8192;
        // How many frames an unused run survives. Long enough that switching screens and coming
        // back is free, short enough that a document nobody is looking at stops costing memory.
        constexpr u64 kIdleFrames = 240;

        struct Cache {
            // Multimap: one hash can hold two different strings, and the compare below decides.
            std::unordered_multimap<u64, Entry> entries;
            u64 frame = 0;
            TextCache::Stats stats;
        };

        Cache& Store() {
            static Cache cache;
            return cache;
        }

    }

    const TextLayoutResult& TextCache::Layout(std::string_view utf8, const TextStyle& style,
                                              f32 maxWidth, WrapMode wrap, TextAlign align) {
        Cache& cache = Store();
        const StyleKey key = KeyOf(style, maxWidth, wrap, align);
        const u64 hash = HashOf(utf8, key);

        auto [first, last] = cache.entries.equal_range(hash);
        for (auto it = first; it != last; ++it) {
            if (it->second.key != key || it->second.text != utf8) continue;
            it->second.touched = cache.frame;
            ++cache.stats.hits;
            return it->second.result;
        }

        ++cache.stats.misses;
        // Full, and nothing has swept it: drop the idle half rather than refusing to cache. A
        // caller that never sweeps still gets a working cache, just a smaller one.
        if (cache.entries.size() >= kMaxEntries) Sweep();
        if (cache.entries.size() >= kMaxEntries) {
            cache.entries.clear();
            ++cache.stats.evictions;
        }

        Entry entry;
        entry.text = std::string(utf8);
        entry.key = key;
        entry.result = TextLayout::Layout(utf8, style, maxWidth, wrap, align);
        entry.touched = cache.frame;
        auto placed = cache.entries.emplace(hash, std::move(entry));
        cache.stats.entries = cache.entries.size();
        return placed->second.result;
    }

    Vec2 TextCache::Measure(std::string_view utf8, const TextStyle& style, f32 maxWidth,
                            WrapMode wrap) {
        if (!style.font) return TextLayout::Measure(utf8, style, maxWidth, wrap);
        return Layout(utf8, style, maxWidth, wrap).size;
    }

    void TextCache::Sweep() {
        Cache& cache = Store();
        ++cache.frame;
        if (cache.frame <= kIdleFrames) return;

        const u64 cutoff = cache.frame - kIdleFrames;
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ) {
            if (it->second.touched < cutoff) {
                it = cache.entries.erase(it);
                ++cache.stats.evictions;
            } else {
                ++it;
            }
        }
        cache.stats.entries = cache.entries.size();
    }

    void TextCache::Clear() {
        Cache& cache = Store();
        cache.entries.clear();
        cache.stats.entries = 0;
    }

    const TextCache::Stats& TextCache::Report() { return Store().stats; }

}
