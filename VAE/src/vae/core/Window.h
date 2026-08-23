#pragma once

#include "vae/base/Base.h"
#include "vae/core/Event.h"

#include <functional>
#include <vector>
#include <filesystem>
#include <string>

namespace vae {

    // Kept here rather than in vae::ui so the window backend does not have to depend on the widget
    // layer to change a cursor. ui::CursorShape maps onto it one for one.
    // ui::CursorShape maps onto it one for one, so new shapes are appended, never inserted.
    enum class Cursor : u8 { Arrow, Hand, IBeam, ResizeH, ResizeV, Crosshair, NotAllowed,
                             ResizeNWSE, ResizeNESW, ResizeAll };

    struct WindowSpec {
        std::string title  = "VAE";
        u32 width          = 1600;
        u32 height         = 900;
        bool vsync         = true;
        bool visible       = true;   // false = created but never shown; how --selftest runs headless
        bool resizable     = true;
        // X11 WM_CLASS. Fixed so `vc` can address the window by class regardless of its title.
        std::string wmClass = "VAE";
    };

    class Window {
    public:
        using EventCallback = std::function<void(Event&)>;
        // Files dropped onto the window from the desktop. Not an Event, because Event is a POD
        // tagged union and a list of paths is neither POD nor one value.
        using FileDropCallback = std::function<void(const std::vector<std::filesystem::path>&)>;

        virtual ~Window() = default;

        virtual void OnUpdate() = 0;                 // pump events + present bookkeeping
        virtual u32  Width()  const = 0;
        virtual u32  Height() const = 0;
        virtual f32  ContentScale() const = 0;       // HiDPI factor; layout works in logical px
        virtual bool ShouldClose() const = 0;
        // Take back a close the window manager already accepted. A layer that vetoes one — an
        // unsaved document asking first — has to clear the flag, or the next frame closes anyway.
        virtual void SetShouldClose(bool) = 0;
        virtual bool Minimized() const = 0;

        virtual void SetEventCallback(const EventCallback&) = 0;
        virtual void SetFileDropCallback(const FileDropCallback&) = 0;
        virtual void SetVSync(bool) = 0;
        virtual bool VSync() const = 0;
        virtual void SetTitle(std::string_view) = 0;
        virtual void SetCursor(Cursor) = 0;

        virtual void SetClipboardText(const std::string&) = 0;
        virtual std::string ClipboardText() const = 0;

        virtual void* NativeHandle() const = 0;      // GLFWwindow* on desktop

        // Blocks until an event arrives or the timeout elapses. An app that is sitting still must
        // not spin the GPU, so the main loop waits rather than polls when nothing is animating.
        virtual void WaitEvents(f64 timeoutSeconds) = 0;
        virtual void PostEmptyEvent() = 0;

        static Scope<Window> Create(const WindowSpec& spec = {});
    };

}
