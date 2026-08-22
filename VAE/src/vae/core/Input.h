#pragma once

#include "vae/base/Base.h"
#include "vae/base/Math.h"
#include "vae/core/KeyCodes.h"

namespace vae {

    // Polled state, for things like "is shift held while dragging". Discrete presses come through
    // events — polling for those loses inputs that happen between frames.
    class Input {
    public:
        static bool IsKeyPressed(Key key);
        static bool IsMouseButtonPressed(Mouse button);
        static Vec2 MousePosition();
        static u16  Modifiers();

        static void SetClipboardText(std::string_view text);
        static std::string ClipboardText();
    };

}
