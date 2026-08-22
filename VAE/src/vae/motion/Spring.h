#pragma once

#include "vae/base/Base.h"

namespace vae::motion {

    // A damped spring, parameterised the way a designer thinks about one: how long it takes to
    // settle and how much it overshoots. Mass, stiffness and damping are what the solver wants, not
    // what anyone wants to type — so they are derived here from the two numbers that mean something.
    //
    // This is the parameterisation SwiftUI settled on for the same reason, and it is the difference
    // between "0.4 seconds, a little bouncy" and guessing at a stiffness of 180.
    struct Spring {
        f32 duration = 0.4f;    // seconds to settle
        f32 bounce = 0.0f;      // 0 = no overshoot · >0 springy · <0 sluggish. Sane range [-1, 1].

        // Angular frequency and damping ratio, which is what the integrator actually needs.
        f32 Omega() const;
        f32 Damping() const;
    };

    // One spring in flight. Integrated semi-implicitly at a fixed sub-step so the result is the same
    // whatever frame rate it is fed — a 30fps machine and a 144fps machine must agree, or a spring
    // is a per-machine animation.
    class SpringMotion {
    public:
        SpringMotion() = default;
        SpringMotion(f32 from, f32 to, Spring spring)
            : m_Value(from), m_Target(to), m_Spring(spring) {}

        void Retarget(f32 to) { m_Target = to; }
        // Keeps the current velocity, which is what makes an interruption feel like a redirection
        // rather than a restart.
        void Reset(f32 value, f32 velocity = 0.0f) { m_Value = value; m_Velocity = velocity; }

        void Advance(f32 dt);
        bool Settled() const;

        f32 Value() const { return m_Value; }
        f32 Velocity() const { return m_Velocity; }
        f32 Target() const { return m_Target; }

    private:
        f32 m_Value = 0.0f;
        f32 m_Target = 0.0f;
        f32 m_Velocity = 0.0f;
        Spring m_Spring{};
    };

}
