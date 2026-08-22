#include "vaepch.h"
#include "vae/motion/Easing.h"

#include <array>
#include <cmath>

namespace vae::motion {

    namespace {

        constexpr f32 kPi = 3.14159265358979323846f;

        // Penner's constants, the values every implementation uses. Named so the arithmetic below
        // reads as "overshoot by about 10%" rather than as three magic decimals.
        constexpr f32 kBackOvershoot = 1.70158f;
        constexpr f32 kBackOvershootInOut = kBackOvershoot * 1.525f;

        f32 OutBounceAt(f32 t) {
            constexpr f32 n = 7.5625f;
            constexpr f32 d = 2.75f;
            if (t < 1.0f / d)      return n * t * t;
            if (t < 2.0f / d)    { t -= 1.5f / d;   return n * t * t + 0.75f; }
            if (t < 2.5f / d)    { t -= 2.25f / d;  return n * t * t + 0.9375f; }
            t -= 2.625f / d;
            return n * t * t + 0.984375f;
        }

        f32 Power(f32 t, int exponent, bool in, bool out) {
            const auto pow_ = [&](f32 x) {
                f32 result = 1.0f;
                for (int i = 0; i < exponent; ++i) result *= x;
                return result;
            };
            if (in && out)
                return t < 0.5f ? pow_(2.0f * t) / 2.0f
                                : 1.0f - pow_(-2.0f * t + 2.0f) / 2.0f;
            if (in)  return pow_(t);
            return 1.0f - pow_(1.0f - t);
        }

        constexpr std::array<const char*, static_cast<std::size_t>(Easing::Count)> kNames{
            "linear",
            "inSine",    "outSine",    "inOutSine",
            "inQuad",    "outQuad",    "inOutQuad",
            "inCubic",   "outCubic",   "inOutCubic",
            "inQuart",   "outQuart",   "inOutQuart",
            "inExpo",    "outExpo",    "inOutExpo",
            "inCirc",    "outCirc",    "inOutCirc",
            "inBack",    "outBack",    "inOutBack",
            "inElastic", "outElastic", "inOutElastic",
            "inBounce",  "outBounce",  "inOutBounce",
        };

    }

    f32 Ease(Easing curve, f32 t) {
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

        switch (curve) {
            case Easing::Linear: return t;

            case Easing::InSine:    return 1.0f - std::cos(t * kPi / 2.0f);
            case Easing::OutSine:   return std::sin(t * kPi / 2.0f);
            case Easing::InOutSine: return -(std::cos(kPi * t) - 1.0f) / 2.0f;

            case Easing::InQuad:     return Power(t, 2, true, false);
            case Easing::OutQuad:    return Power(t, 2, false, true);
            case Easing::InOutQuad:  return Power(t, 2, true, true);
            case Easing::InCubic:    return Power(t, 3, true, false);
            case Easing::OutCubic:   return Power(t, 3, false, true);
            case Easing::InOutCubic: return Power(t, 3, true, true);
            case Easing::InQuart:    return Power(t, 4, true, false);
            case Easing::OutQuart:   return Power(t, 4, false, true);
            case Easing::InOutQuart: return Power(t, 4, true, true);

            case Easing::InExpo:  return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
            case Easing::OutExpo: return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
            case Easing::InOutExpo:
                if (t == 0.0f) return 0.0f;
                if (t == 1.0f) return 1.0f;
                return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
                                : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) / 2.0f;

            case Easing::InCirc:  return 1.0f - std::sqrt(1.0f - t * t);
            case Easing::OutCirc: return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f));
            case Easing::InOutCirc:
                return t < 0.5f
                     ? (1.0f - std::sqrt(1.0f - 4.0f * t * t)) / 2.0f
                     : (std::sqrt(1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f)) + 1.0f) / 2.0f;

            case Easing::InBack:
                return (kBackOvershoot + 1.0f) * t * t * t - kBackOvershoot * t * t;
            case Easing::OutBack: {
                const f32 u = t - 1.0f;
                return 1.0f + (kBackOvershoot + 1.0f) * u * u * u + kBackOvershoot * u * u;
            }
            case Easing::InOutBack:
                return t < 0.5f
                     ? (std::pow(2.0f * t, 2.0f)
                        * ((kBackOvershootInOut + 1.0f) * 2.0f * t - kBackOvershootInOut)) / 2.0f
                     : (std::pow(2.0f * t - 2.0f, 2.0f)
                        * ((kBackOvershootInOut + 1.0f) * (t * 2.0f - 2.0f) + kBackOvershootInOut)
                        + 2.0f) / 2.0f;

            case Easing::InElastic:
                if (t == 0.0f) return 0.0f;
                if (t == 1.0f) return 1.0f;
                return -std::pow(2.0f, 10.0f * t - 10.0f)
                     * std::sin((t * 10.0f - 10.75f) * (2.0f * kPi / 3.0f));
            case Easing::OutElastic:
                if (t == 0.0f) return 0.0f;
                if (t == 1.0f) return 1.0f;
                return std::pow(2.0f, -10.0f * t)
                     * std::sin((t * 10.0f - 0.75f) * (2.0f * kPi / 3.0f)) + 1.0f;
            case Easing::InOutElastic:
                if (t == 0.0f) return 0.0f;
                if (t == 1.0f) return 1.0f;
                return t < 0.5f
                     ? -(std::pow(2.0f, 20.0f * t - 10.0f)
                         * std::sin((20.0f * t - 11.125f) * (2.0f * kPi / 4.5f))) / 2.0f
                     : (std::pow(2.0f, -20.0f * t + 10.0f)
                        * std::sin((20.0f * t - 11.125f) * (2.0f * kPi / 4.5f))) / 2.0f + 1.0f;

            case Easing::InBounce:  return 1.0f - OutBounceAt(1.0f - t);
            case Easing::OutBounce: return OutBounceAt(t);
            case Easing::InOutBounce:
                return t < 0.5f ? (1.0f - OutBounceAt(1.0f - 2.0f * t)) / 2.0f
                                : (1.0f + OutBounceAt(2.0f * t - 1.0f)) / 2.0f;

            default: return t;
        }
    }

    const char* EasingName(Easing curve) {
        const auto index = static_cast<std::size_t>(curve);
        return index < kNames.size() ? kNames[index] : "linear";
    }

    std::optional<Easing> EasingFromName(std::string_view name) {
        for (std::size_t i = 0; i < kNames.size(); ++i)
            if (name == kNames[i]) return static_cast<Easing>(i);
        return std::nullopt;
    }

}
