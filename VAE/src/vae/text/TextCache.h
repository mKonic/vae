#pragma once

#include "vae/text/TextLayout.h"

#include <string>
#include <unordered_map>

namespace vae::text {

    // Shaped runs, kept between frames.
    //
    // Shaping is the most expensive thing a UI frame does and the least likely to have changed:
    // measured on a 6001-node screen, 4000 labels cost 40.6 ms a frame against 1.3 ms for the same
    // node count with no text in it — ~10 µs per label per frame, every frame, for a string nobody
    // touched. Worse, a repeated container draws the same label with the same style in every copy,
    // so a list of a hundred rows shapes "Hello" a hundred times.
    //
    // The cache is keyed by everything that changes the answer — the text, the face, the size and
    // spacing, the width it was wrapped into, the wrap mode and the alignment — and returns a
    // reference into itself, so measuring costs a hash and a string compare and painting costs
    // nothing at all.
    class TextCache {
    public:
        // Layout, from the cache when it can be. The reference stays valid until the next Sweep,
        // which is a frame boundary; nothing here is meant to be held across one.
        static const TextLayoutResult& Layout(std::string_view utf8, const TextStyle& style,
                                              f32 maxWidth = 0.0f,
                                              WrapMode wrap = WrapMode::Word,
                                              TextAlign align = TextAlign::Left);

        // Just the box, which is what the layout solver asks for. Same cache.
        static Vec2 Measure(std::string_view utf8, const TextStyle& style, f32 maxWidth = 0.0f,
                            WrapMode wrap = WrapMode::Word);

        // End of a frame: entries nothing asked for in a while go. Called by the application loop;
        // a headless caller that never calls it is bounded by kMaxEntries instead.
        static void Sweep();
        // Fonts changed underneath us — a face was registered, the atlas was rebuilt. Every shaped
        // run is now against a face that may not exist.
        static void Clear();

        struct Stats {
            u64 hits = 0;
            u64 misses = 0;
            u64 evictions = 0;
            std::size_t entries = 0;
        };
        static const Stats& Report();
    };

}
