#pragma once

#include "vae/base/Base.h"

#include <optional>
#include <string_view>

namespace vae::motion {

    // The curves a designer actually reaches for, named the way the web names them so the names
    // transfer. Anything exotic is a spring, and springs are the other half of this file.
    enum class Easing : u8 {
        Linear,
        InSine,    OutSine,    InOutSine,
        InQuad,    OutQuad,    InOutQuad,
        InCubic,   OutCubic,   InOutCubic,
        InQuart,   OutQuart,   InOutQuart,
        InExpo,    OutExpo,    InOutExpo,
        InCirc,    OutCirc,    InOutCirc,
        InBack,    OutBack,    InOutBack,
        InElastic, OutElastic, InOutElastic,
        InBounce,  OutBounce,  InOutBounce,
        Count
    };

    // t is clamped to [0, 1]. Every curve returns 0 at 0 and 1 at 1 — including the ones that
    // overshoot in between, which is the whole reason to use them.
    f32 Ease(Easing curve, f32 t);

    const char* EasingName(Easing curve);
    std::optional<Easing> EasingFromName(std::string_view name);

}
