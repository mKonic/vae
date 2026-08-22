#pragma once

#include "vae/base/Base.h"
#include "vae/base/Math.h"
#include "vae/core/KeyCodes.h"

namespace vae {

    enum class EventType : u8 {
        None = 0,
        WindowClose, WindowResize, WindowFocus, WindowLostFocus, WindowMove, WindowScaleChanged,
        KeyPressed, KeyReleased, TextInput,
        MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled, MouseEnter, MouseLeave,
        FileDropped,
    };

    // One POD struct with a tagged union rather than a class hierarchy. Events are produced in
    // window callbacks and consumed in the same frame; a heap allocation and a vtable per mouse-move
    // is a cost a UI engine has no reason to pay, and the flat layout forwards to the script ABI
    // without a translation step.
    struct Event {
        EventType type = EventType::None;
        bool handled = false;
        u16 mods = Mod::None;

        union {
            struct { u32 width, height; } size;
            struct { i32 x, y; } pos;
            struct { Key code; bool repeat; } key;
            struct { u32 codepoint; } text;
            struct { Mouse button; f32 x, y; } button;
            struct { f32 x, y; } mouse;
            struct { f32 dx, dy; } scroll;
            struct { f32 x, y; } scale;
            struct { const char* const* paths; u32 count; } drop;
        };

        constexpr Event() : mouse{0.0f, 0.0f} {}
        constexpr explicit Event(EventType t) : type(t), mouse{0.0f, 0.0f} {}

        constexpr bool IsMouse() const {
            return type == EventType::MouseButtonPressed || type == EventType::MouseButtonReleased
                || type == EventType::MouseMoved || type == EventType::MouseScrolled;
        }
        constexpr bool IsKeyboard() const {
            return type == EventType::KeyPressed || type == EventType::KeyReleased
                || type == EventType::TextInput;
        }
    };

    // Helpers so call sites read as statements rather than aggregate initialization.
    inline Event MakeWindowResize(u32 w, u32 h) { Event e(EventType::WindowResize); e.size = {w, h}; return e; }
    inline Event MakeKey(EventType t, Key k, u16 mods, bool repeat = false) {
        Event e(t); e.mods = mods; e.key = {k, repeat}; return e;
    }
    inline Event MakeMouseMoved(f32 x, f32 y) { Event e(EventType::MouseMoved); e.mouse = {x, y}; return e; }
    inline Event MakeMouseButton(EventType t, Mouse b, f32 x, f32 y, u16 mods) {
        Event e(t); e.mods = mods; e.button = {b, x, y}; return e;
    }
    inline Event MakeScroll(f32 dx, f32 dy, u16 mods) { Event e(EventType::MouseScrolled); e.mods = mods; e.scroll = {dx, dy}; return e; }
    inline Event MakeTextInput(u32 codepoint) { Event e(EventType::TextInput); e.text = {codepoint}; return e; }

}
