#include "vaepch.h"
#include "vae/core/Input.h"

#include "vae/core/Application.h"

#include <GLFW/glfw3.h>

namespace vae {

    namespace {
        GLFWwindow* Handle() {
            return static_cast<GLFWwindow*>(Application::Get().GetWindow().NativeHandle());
        }
    }

    bool Input::IsKeyPressed(Key key) {
        return glfwGetKey(Handle(), static_cast<int>(key)) == GLFW_PRESS;
    }

    bool Input::IsMouseButtonPressed(Mouse button) {
        return glfwGetMouseButton(Handle(), static_cast<int>(button)) == GLFW_PRESS;
    }

    Vec2 Input::MousePosition() {
        double x = 0.0, y = 0.0;
        glfwGetCursorPos(Handle(), &x, &y);
        return { static_cast<f32>(x), static_cast<f32>(y) };
    }

    u16 Input::Modifiers() {
        u16 mods = Mod::None;
        auto down = [](Key k) { return Input::IsKeyPressed(k); };
        if (down(Key::LeftShift)   || down(Key::RightShift))   mods |= Mod::Shift;
        if (down(Key::LeftControl) || down(Key::RightControl)) mods |= Mod::Control;
        if (down(Key::LeftAlt)     || down(Key::RightAlt))     mods |= Mod::Alt;
        if (down(Key::LeftSuper)   || down(Key::RightSuper))   mods |= Mod::Super;
        return mods;
    }

    void Input::SetClipboardText(std::string_view text) {
        glfwSetClipboardString(Handle(), std::string(text).c_str());
    }

    std::string Input::ClipboardText() {
        const char* s = glfwGetClipboardString(Handle());
        return s ? std::string(s) : std::string{};
    }

}
