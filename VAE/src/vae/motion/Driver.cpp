#include "vaepch.h"
#include "vae/motion/Driver.h"

#include <algorithm>
#include <cmath>

namespace vae::motion {

    u32 Lanes(doc::ValueType type) {
        switch (type) {
            case doc::ValueType::Number:  return 1;
            case doc::ValueType::Vector2: return 2;
            case doc::ValueType::Colour:  return 4;
            default:                      return 0;
        }
    }

    void Unpack(const doc::Value& value, f32* out) {
        switch (doc::TypeOf(value)) {
            case doc::ValueType::Number:
                out[0] = std::get<f32>(value);
                break;
            case doc::ValueType::Vector2: {
                const Vec2 v = std::get<Vec2>(value);
                out[0] = v.x; out[1] = v.y;
                break;
            }
            case doc::ValueType::Colour: {
                const Color c = std::get<Color>(value);
                out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a;
                break;
            }
            default: break;
        }
    }

    doc::Value Pack(doc::ValueType type, const f32* lanes) {
        switch (type) {
            case doc::ValueType::Number:  return lanes[0];
            case doc::ValueType::Vector2: return Vec2{ lanes[0], lanes[1] };
            case doc::ValueType::Colour:  return Color{ lanes[0], lanes[1], lanes[2], lanes[3] };
            default:                      return {};
        }
    }

    doc::Value Driver::Track::Sample() const {
        if (lanes == 0) return elapsed >= options.delay + options.duration ? discrete : doc::Value{};

        f32 values[4]{};
        if (spring) {
            for (u32 i = 0; i < lanes; ++i) values[i] = springs[i].Value();
        } else {
            const f32 time = elapsed - options.delay;
            const f32 t = options.duration <= 0.0f
                        ? 1.0f : std::clamp(time / options.duration, 0.0f, 1.0f);
            // Arrived means arrived. `from + (to - from) * 1.0f` is not exactly `to` in floating
            // point, and a colour that lands a hair off its target never compares equal to it again.
            if (t >= 1.0f) {
                for (u32 i = 0; i < lanes; ++i) values[i] = to[i];
            } else {
                const f32 eased = Ease(options.curve, t);
                for (u32 i = 0; i < lanes; ++i) values[i] = from[i] + (to[i] - from[i]) * eased;
            }
        }
        return Pack(type, values);
    }

    bool Driver::Track::Done() const {
        if (elapsed < options.delay) return false;
        if (lanes == 0) return elapsed >= options.delay + options.duration;
        if (!spring) return elapsed - options.delay >= options.duration;
        for (u32 i = 0; i < lanes; ++i) if (!springs[i].Settled()) return false;
        return true;
    }

    void Driver::To(const Key& key, doc::Value from, doc::Value to, const Options& options) {
        const doc::ValueType type = doc::TypeOf(to);
        const u32 lanes = Lanes(type);

        // Nothing to interpolate: a token, a string, a bool. Held at its old value for the duration
        // and then switched, which is at least honest about being a delay rather than a fade.
        if (lanes == 0 || doc::TypeOf(from) != type) {
            Track track;
            track.type = type;
            track.options = options;
            track.discrete = std::move(to);
            m_Running[key] = std::move(track);
            return;
        }

        f32 target[4]{};
        Unpack(to, target);

        // Already running: keep the velocity and aim somewhere else. Restarting from the new `from`
        // would make an interruption look like a glitch.
        if (const auto it = m_Running.find(key); it != m_Running.end() && it->second.lanes == lanes) {
            Track& track = it->second;
            const doc::Value current = track.Sample();
            f32 now[4]{};
            if (doc::IsSet(current)) Unpack(current, now); else Unpack(from, now);

            track.options = options;
            track.spring = options.spring;
            track.elapsed = 0.0f;
            // It may have arrived and be waiting to be dropped. Redirecting it makes it live again,
            // and a track still flagged finished is erased at the top of the next Advance — the new
            // animation would vanish before its first frame.
            track.finished = false;
            for (u32 i = 0; i < lanes; ++i) {
                track.from[i] = now[i];
                track.to[i] = target[i];
                if (options.spring) {
                    const f32 velocity = track.springs[i].Velocity();
                    track.springs[i] = SpringMotion(now[i], target[i], options.physics);
                    track.springs[i].Reset(now[i], velocity);
                }
            }
            return;
        }

        f32 start[4]{};
        Unpack(from, start);

        Track track;
        track.type = type;
        track.lanes = lanes;
        track.options = options;
        track.spring = options.spring;
        for (u32 i = 0; i < lanes; ++i) {
            track.from[i] = start[i];
            track.to[i] = target[i];
            if (options.spring) track.springs[i] = SpringMotion(start[i], target[i], options.physics);
        }
        m_Running[key] = std::move(track);
    }

    doc::Value Driver::Current(const Key& key) const {
        const auto it = m_Running.find(key);
        return it == m_Running.end() ? doc::Value{} : it->second.Sample();
    }

    bool Driver::Animating(const Key& key) const { return m_Running.contains(key); }

    void Driver::Cancel(const Key& key) { m_Running.erase(key); }

    void Driver::CancelOwner(Uuid owner) {
        std::erase_if(m_Running, [&](const auto& entry) { return entry.first.owner == owner; });
    }

    void Driver::Clear() { m_Running.clear(); }

    bool Driver::Busy() const {
        return std::ranges::any_of(m_Running, [](const auto& e) { return !e.second.finished; });
    }

    bool Driver::Advance(f32 dt) {
        // Last frame's finished animations are dropped now, not when they finished. A caller reads
        // after advancing, so erasing on completion would mean the exact final value is the one
        // value nobody ever sees — and an animation that never quite arrives is a bug report.
        std::erase_if(m_Running, [](const auto& entry) { return entry.second.finished; });

        for (auto& [key, track] : m_Running) {
            track.elapsed += dt;
            if (track.spring && track.elapsed >= track.options.delay)
                for (u32 i = 0; i < track.lanes; ++i) track.springs[i].Advance(dt);
            track.finished = track.Done();
        }
        return std::ranges::any_of(m_Running, [](const auto& e) { return !e.second.finished; });
    }

}
