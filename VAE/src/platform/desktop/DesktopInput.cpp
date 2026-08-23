#include "vaepch.h"
#include "vae/core/Input.h"

#include "vae/core/Application.h"

#include <GLFW/glfw3.h>

#include <string>

namespace vae {

    namespace {
        // Null when there is no window: --selftest, --convert and every headless run have input
        // and clipboard calls reaching them through code that does not know it is headless. GLFW
        // treats a null window as "no clipboard", which is the right answer, where dereferencing
        // a null unique_ptr to ask is not.
        GLFWwindow* Handle() {
            Application& app = Application::Get();
            return app.HasWindow() ? static_cast<GLFWwindow*>(app.GetWindow().NativeHandle())
                                   : nullptr;
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

    // Headless keeps its own clipboard rather than reaching for the desktop's. The selftest cuts
    // and pastes exactly the way the editor does, and a copy that quietly went nowhere would make
    // the paste that followed it look like the bug.
    namespace { std::string s_Headless; }

    void Input::SetClipboardText(std::string_view text) {
        if (GLFWwindow* window = Handle()) glfwSetClipboardString(window, std::string(text).c_str());
        else s_Headless = text;
    }

    std::string Input::ClipboardText() {
        GLFWwindow* window = Handle();
        if (!window) return s_Headless;
        const char* s = glfwGetClipboardString(window);
        return s ? std::string(s) : std::string{};
    }

}
