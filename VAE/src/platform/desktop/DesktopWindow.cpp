#include "vaepch.h"
#include "platform/desktop/DesktopWindow.h"

#include "vae/base/FileSystem.h"
#include "vae/vector/Svg.h"

#include <GLFW/glfw3.h>

namespace vae {

    namespace {
        u32  s_WindowCount = 0;
        bool s_Initialised = false;

        // Deduplicated, because a GLFW that is unhappy is unhappy once per call and the engine
        // calls it several times a frame. One startup failure used to write 4.7 MB of the same
        // line before anyone could read the one above it that said why.
        void OnGlfwError(int code, const char* description) {
            static int lastCode = 0;
            static u64 repeats = 0;
            if (code == lastCode) {
                if (++repeats > 3) return;
            } else {
                lastCode = code;
                repeats = 0;
            }
            VAE_CORE_ERROR("GLFW error {}: {}", code, description);
            if (repeats == 3) VAE_CORE_ERROR("GLFW error {}: repeating; further copies suppressed", code);
        }
    }

    // Null when there is no window to be had. Every caller has to cope with that: a machine with
    // no display server is a normal machine to be started on by mistake, not a broken one, and the
    // answer is one clear line and an exit code -- not a crash, and not a loop drawing nothing.
    Scope<Window> Window::Create(const WindowSpec& spec) {
        auto window = CreateScope<DesktopWindow>(spec);
        return window->NativeHandle() ? Scope<Window>(std::move(window)) : nullptr;
    }

    DesktopWindow::DesktopWindow(const WindowSpec& spec) {
        m_Data.width  = spec.width;
        m_Data.height = spec.height;
        m_Data.vsync  = spec.vsync;

        // Never inside an assert: VAE_CORE_ASSERT is ((void)0) in Dist, which compiled the
        // glfwInit() call itself out of the shipped build. The engine then ran its whole startup
        // against an uninitialised GLFW, opened nothing, and spun forever logging error 65537.
        if (s_WindowCount == 0) {
            glfwSetErrorCallback(OnGlfwError);
            if (!glfwInit()) {
                VAE_CORE_ERROR("GLFW could not start — no display server? "
                               "(check WAYLAND_DISPLAY or DISPLAY)");
                return;
            }
            s_Initialised = true;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);          // Vulkan does the drawing
        glfwWindowHint(GLFW_RESIZABLE, spec.resizable ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE,   spec.visible   ? GLFW_TRUE : GLFW_FALSE);

        // Fixed WM_CLASS: `vc` and window rules address the window by class, and the title changes
        // as soon as a project is open. Must be set before the window is created.
        glfwWindowHintString(GLFW_X11_CLASS_NAME,    spec.wmClass.c_str());
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, spec.wmClass.c_str());

        m_Handle = glfwCreateWindow(static_cast<int>(spec.width), static_cast<int>(spec.height),
                                    spec.title.c_str(), nullptr, nullptr);
        if (!m_Handle) {
            VAE_CORE_ERROR("could not create a window — the display server refused it");
            return;
        }
        ++s_WindowCount;

        SetIcon();

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(m_Handle, &fbw, &fbh);
        m_Data.width  = static_cast<u32>(fbw);
        m_Data.height = static_cast<u32>(fbh);

        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(m_Handle, &sx, &sy);
        m_Data.scale = sx;

        glfwSetWindowUserPointer(m_Handle, &m_Data);
        InstallCallbacks();
        CreateCursors();
    }

    DesktopWindow::~DesktopWindow() {
        for (GLFWcursor*& cursor : m_Cursors) {
            if (cursor) glfwDestroyCursor(cursor);
            cursor = nullptr;
        }
        if (m_Handle) { glfwDestroyWindow(m_Handle); --s_WindowCount; }
        if (s_WindowCount == 0 && s_Initialised) { glfwTerminate(); s_Initialised = false; }
    }

    void DesktopWindow::InstallCallbacks() {
        auto emit = [](GLFWwindow* w, Event e) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            if (data.callback) data.callback(e);
        };

        glfwSetFramebufferSizeCallback(m_Handle, [](GLFWwindow* w, int width, int height) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            data.width  = static_cast<u32>(width);
            data.height = static_cast<u32>(height);
            Event e = MakeWindowResize(data.width, data.height);
            if (data.callback) data.callback(e);
        });

        glfwSetWindowCloseCallback(m_Handle, [](GLFWwindow* w) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            Event e(EventType::WindowClose);
            if (data.callback) data.callback(e);
        });

        glfwSetWindowFocusCallback(m_Handle, [](GLFWwindow* w, int focused) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            Event e(focused ? EventType::WindowFocus : EventType::WindowLostFocus);
            if (data.callback) data.callback(e);
        });

        glfwSetWindowContentScaleCallback(m_Handle, [](GLFWwindow* w, float sx, float sy) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            data.scale = sx;
            Event e(EventType::WindowScaleChanged);
            e.scale = { sx, sy };
            if (data.callback) data.callback(e);
        });

        glfwSetKeyCallback(m_Handle, [](GLFWwindow* w, int key, int, int action, int mods) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            if (action == GLFW_RELEASE) {
                Event e = MakeKey(EventType::KeyReleased, static_cast<Key>(key), static_cast<u16>(mods));
                if (data.callback) data.callback(e);
            } else {
                Event e = MakeKey(EventType::KeyPressed, static_cast<Key>(key),
                                  static_cast<u16>(mods), action == GLFW_REPEAT);
                if (data.callback) data.callback(e);
            }
        });

        // Files dragged in from the desktop. GLFW hands them over as UTF-8 paths and owns the
        // array only for the duration of the call, so they are copied out before anything else.
        glfwSetDropCallback(m_Handle, [](GLFWwindow* w, int count, const char** paths) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            if (!data.drop || count <= 0) return;
            std::vector<std::filesystem::path> files;
            files.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) files.emplace_back(paths[i]);
            data.drop(files);
        });

        glfwSetCharCallback(m_Handle, [](GLFWwindow* w, unsigned int codepoint) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            Event e = MakeTextInput(codepoint);
            if (data.callback) data.callback(e);
        });

        glfwSetMouseButtonCallback(m_Handle, [](GLFWwindow* w, int button, int action, int mods) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            double x = 0.0, y = 0.0;
            glfwGetCursorPos(w, &x, &y);
            Event e = MakeMouseButton(action == GLFW_PRESS ? EventType::MouseButtonPressed
                                                           : EventType::MouseButtonReleased,
                                      static_cast<Mouse>(button),
                                      static_cast<f32>(x), static_cast<f32>(y), static_cast<u16>(mods));
            if (data.callback) data.callback(e);
        });

        glfwSetCursorPosCallback(m_Handle, [](GLFWwindow* w, double x, double y) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            Event e = MakeMouseMoved(static_cast<f32>(x), static_cast<f32>(y));
            if (data.callback) data.callback(e);
        });

        glfwSetScrollCallback(m_Handle, [](GLFWwindow* w, double dx, double dy) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            Event e = MakeScroll(static_cast<f32>(dx), static_cast<f32>(dy), Mod::None);
            if (data.callback) data.callback(e);
        });

        glfwSetCursorEnterCallback(m_Handle, [](GLFWwindow* w, int entered) {
            auto& data = *static_cast<Data*>(glfwGetWindowUserPointer(w));
            Event e(entered ? EventType::MouseEnter : EventType::MouseLeave);
            if (data.callback) data.callback(e);
        });

        (void)emit;
    }

    void DesktopWindow::OnUpdate() { glfwPollEvents(); }

    bool DesktopWindow::ShouldClose() const { return !m_Handle || glfwWindowShouldClose(m_Handle); }

    void DesktopWindow::SetShouldClose(bool should) {
        glfwSetWindowShouldClose(m_Handle, should ? GLFW_TRUE : GLFW_FALSE);
    }

    // Rasterized from the engine's own SVG at startup, rather than shipping a pile of PNGs at four
    // sizes. Wayland ignores this — a compositor takes the icon from the .desktop file, which is
    // why WM_CLASS is set — but X11, and every screenshot of an X11 session, uses it.
    void DesktopWindow::SetIcon() {
        // Asking anyway is not harmless: GLFW answers with an error every single start, and a
        // shipped app whose first log line is an error is a shipped app people file bugs about.
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) return;

        const auto source = FileSystem::ReadText(FileSystem::Asset("VAE/assets/icon.svg"));
        if (!source) return;

        vector::Picture picture;
        if (!vector::ParseSvg(*source, picture) || picture.Empty()) return;

        // Two sizes, so a window list and a task switcher each pick the one they want.
        vector::Bitmap large = vector::Render(picture, 64, 64);
        vector::Bitmap small = vector::Render(picture, 32, 32);
        if (large.Empty() || small.Empty()) return;

        const GLFWimage images[2] = {
            { static_cast<int>(large.width), static_cast<int>(large.height), large.pixels.data() },
            { static_cast<int>(small.width), static_cast<int>(small.height), small.pixels.data() },
        };
        glfwSetWindowIcon(m_Handle, 2, images);
    }

    void DesktopWindow::SetTitle(std::string_view title) {
        glfwSetWindowTitle(m_Handle, std::string(title).c_str());
    }

    void DesktopWindow::CreateCursors() {
        // Index by vae::Cursor. GLFW 3.4 added the resize and not-allowed shapes; on a compositor
        // that has no theme entry for one, glfwCreateStandardCursor returns null and the arrow
        // stays, which is the right degradation.
        static constexpr int kShapes[kCursorCount] = {
            GLFW_ARROW_CURSOR, GLFW_POINTING_HAND_CURSOR, GLFW_IBEAM_CURSOR,
            GLFW_RESIZE_EW_CURSOR, GLFW_RESIZE_NS_CURSOR, GLFW_CROSSHAIR_CURSOR,
            GLFW_NOT_ALLOWED_CURSOR, GLFW_RESIZE_NWSE_CURSOR, GLFW_RESIZE_NESW_CURSOR,
            GLFW_RESIZE_ALL_CURSOR,
        };
        for (std::size_t i = 0; i < kCursorCount; ++i)
            m_Cursors[i] = glfwCreateStandardCursor(kShapes[i]);
    }

    void DesktopWindow::SetCursor(Cursor cursor) {
        if (cursor == m_Cursor) return;
        m_Cursor = cursor;
        const auto index = static_cast<std::size_t>(cursor);
        if (index >= kCursorCount) return;
        glfwSetCursor(m_Handle, m_Cursors[index]);
    }

    void DesktopWindow::SetClipboardText(const std::string& text) {
        glfwSetClipboardString(m_Handle, text.c_str());
    }

    std::string DesktopWindow::ClipboardText() const {
        const char* text = glfwGetClipboardString(m_Handle);
        return text ? std::string(text) : std::string();
    }

    void DesktopWindow::WaitEvents(f64 timeoutSeconds) { glfwWaitEventsTimeout(timeoutSeconds); }

    void DesktopWindow::PostEmptyEvent() { glfwPostEmptyEvent(); }

}
