#pragma once

#include "vae/doc/Value.h"
#include "vae/motion/Easing.h"
#include "vae/motion/Spring.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vae::motion {

    // What is being animated. Deliberately opaque to the driver: it is a widget, a property and an
    // optional discriminator, and the driver's only job is that two animations of the same thing
    // replace each other rather than fight.
    struct Key {
        Uuid owner = Uuid::Invalid();     // the view's instance, or the node itself
        Uuid node = Uuid::Invalid();
        u16  prop = 0;
        u16  slot = 0;                    // for anything a property alone does not distinguish

        auto operator<=>(const Key&) const = default;
    };

    struct Options {
        f32 duration = 0.2f;
        f32 delay = 0.0f;
        Easing curve = Easing::OutCubic;
        // When set, the curve is ignored and a spring is used instead. Springs interrupt gracefully;
        // curves restart. That is the real difference, not the shape.
        bool spring = false;
        Spring physics{};
    };

    // Every animation in flight, and the values they are currently at.
    //
    // Animated values never reach the document. An animation is a presentation fact — it is what the
    // widget looks like right now, not what the designer drew — and writing the document every frame
    // would rebuild the view tree every frame and make the app unclickable.
    class Driver {
    public:
        // Starts, or redirects one already running. Redirecting a spring keeps its velocity, which
        // is what makes an interrupted animation feel like it changed its mind rather than restarted.
        void To(const Key& key, doc::Value from, doc::Value to, const Options& options);
        // The value it should show right now, or unset if nothing is animating this key.
        doc::Value Current(const Key& key) const;
        bool Animating(const Key& key) const;

        void Cancel(const Key& key);
        void CancelOwner(Uuid owner);
        void Clear();

        // Advances everything. Returns true while anything is still moving, which is what tells an
        // idle main loop it needs another frame.
        bool Advance(f32 dt);
        // Whether anything is still moving. A track that has arrived is kept for one more frame so
        // its final value can be read, and does not count as busy.
        bool Busy() const;
        std::size_t Count() const { return m_Running.size(); }

    private:
        // A value is animated as up to four scalars: a float is one, a Vec2 two, a colour four.
        // Anything else — text, a token reference — cannot be interpolated and is set at the end.
        struct Track {
            doc::ValueType type = doc::ValueType::Unset;
            u32 lanes = 0;
            f32 from[4]{};
            f32 to[4]{};
            SpringMotion springs[4]{};

            Options options{};
            f32 elapsed = 0.0f;
            bool spring = false;
            bool finished = false;        // done, and its final value already readable
            doc::Value discrete;          // for a type with nothing to interpolate

            doc::Value Sample() const;
            bool Done() const;
        };

        std::map<Key, Track> m_Running;
    };

    // Both ends of an interpolation, if they can be interpolated at all.
    u32 Lanes(doc::ValueType type);
    void Unpack(const doc::Value& value, f32* out);
    doc::Value Pack(doc::ValueType type, const f32* lanes);

}
