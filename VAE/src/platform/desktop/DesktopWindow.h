#pragma once

#include "vae/core/Window.h"

struct GLFWwindow;
struct GLFWcursor;

namespace vae {

    class DesktopWindow final : public Window {
    public:
        explicit DesktopWindow(const WindowSpec& spec);
        ~DesktopWindow() override;

        void OnUpdate() override;
        u32  Width()  const override { return m_Data.width; }
        u32  Height() const override { return m_Data.height; }
        f32  ContentScale() const override { return m_Data.scale; }
        bool ShouldClose() const override;
        void SetShouldClose(bool) override;
        bool Minimized() const override { return m_Data.width == 0 || m_Data.height == 0; }

        void SetEventCallback(const EventCallback& cb) override { m_Data.callback = cb; }
        void SetVSync(bool on) override { m_Data.vsync = on; }
        bool VSync() const override { return m_Data.vsync; }
        void SetTitle(std::string_view title) override;
        void SetCursor(Cursor cursor) override;

        void SetClipboardText(const std::string& text) override;
        std::string ClipboardText() const override;

        void* NativeHandle() const override { return m_Handle; }

        void WaitEvents(f64 timeoutSeconds) override;
        void PostEmptyEvent() override;

    private:
        void InstallCallbacks();
        void CreateCursors();

        // Handed to GLFW as the window user pointer; every callback reaches the engine through it.
        struct Data {
            u32 width = 0, height = 0;
            f32 scale = 1.0f;
            bool vsync = true;
            EventCallback callback;
        };

        GLFWwindow* m_Handle = nullptr;
        Data        m_Data;
        // Created once and kept: GLFW's standard cursors are cheap, but re-creating one per hover
        // change flickers on X11.
        static constexpr std::size_t kCursorCount = 10;
        GLFWcursor* m_Cursors[kCursorCount]{};
        Cursor      m_Cursor = Cursor::Arrow;
    };

}
