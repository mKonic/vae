#pragma once

#include "vae/base/Base.h"

namespace vae {

    // Seconds since the previous frame. A UI engine spends most of its life idle, so callers must
    // cope with a large step after a wake-up; the animation driver clamps rather than extrapolating.
    class Timestep {
    public:
        constexpr Timestep(f32 seconds = 0.0f) : m_Seconds(seconds) {}

        constexpr operator f32() const { return m_Seconds; }
        constexpr f32 Seconds() const { return m_Seconds; }
        constexpr f32 Millis()  const { return m_Seconds * 1000.0f; }

    private:
        f32 m_Seconds;
    };

}
