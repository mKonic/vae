#include "vaepch.h"
#include "vae/motion/Spring.h"

#include <algorithm>
#include <cmath>

namespace vae::motion {

    namespace {
        // The fixed integration step. Small enough that a stiff spring stays stable, and fixed so
        // the same animation fed 16.6ms and 6.9ms lands in the same place.
        constexpr f32 kStep = 1.0f / 480.0f;
        constexpr f32 kSettleValue = 0.001f;
        constexpr f32 kSettleVelocity = 0.01f;
    }

    // A settling time of `duration` at this damping. The 6.0 comes from asking when e^(-ζωt) has
    // decayed to ~0.25% — the point where nothing on screen is still visibly moving.
    f32 Spring::Omega() const {
        const f32 seconds = std::max(duration, 0.0001f);
        return 6.0f / (seconds * std::max(Damping(), 0.1f));
    }

    f32 Spring::Damping() const {
        // bounce 0 is critical damping. Above it the ratio drops toward zero (springy); below it,
        // the ratio climbs past one (overdamped, no overshoot at all).
        const f32 clamped = std::clamp(bounce, -1.0f, 1.0f);
        return clamped >= 0.0f ? 1.0f - clamped * 0.75f : 1.0f - clamped * 1.5f;
    }

    void SpringMotion::Advance(f32 dt) {
        if (dt <= 0.0f) return;

        const f32 omega = m_Spring.Omega();
        const f32 zeta = m_Spring.Damping();

        // Sub-stepped rather than solved analytically, because the target can move mid-flight and a
        // closed form would have to be restarted every time it does.
        f32 remaining = std::min(dt, 1.0f);      // a long stall must not launch the spring
        while (remaining > 0.0f) {
            const f32 step = std::min(kStep, remaining);
            remaining -= step;

            const f32 displacement = m_Value - m_Target;
            const f32 acceleration = -omega * omega * displacement - 2.0f * zeta * omega * m_Velocity;
            m_Velocity += acceleration * step;
            m_Value += m_Velocity * step;
        }

        if (Settled()) {
            m_Value = m_Target;
            m_Velocity = 0.0f;
        }
    }

    bool SpringMotion::Settled() const {
        // Relative to the distance travelled, so a spring over 400 pixels and one over 0.4 opacity
        // both stop when they look stopped.
        const f32 scale = std::max(1.0f, std::abs(m_Target));
        return std::abs(m_Value - m_Target) < kSettleValue * scale
            && std::abs(m_Velocity) < kSettleVelocity * scale;
    }

}
