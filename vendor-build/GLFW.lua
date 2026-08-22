-- GLFW static lib from the upstream submodule.
--
-- BOTH the X11 and Wayland backends are compiled in; GLFW picks one at runtime (Wayland when
-- WAYLAND_DISPLAY is set). Wayland is not optional here: `vc` test boxes deliberately unset DISPLAY
-- so an X11 client cannot leak its window into the host session, so an X11-only build cannot be
-- screenshot-tested at all. It is also the correct backend on a Hyprland desktop.
--
-- The Wayland backend #includes protocol headers that wayland-scanner generates; GLFW's own CMake
-- does that, and since we don't use its CMake, scripts/GenWaylandProtocols.sh does it instead into
-- VAE/vendor-generated/ (outside the submodule, so the submodule stays clean).
--
-- Explicit source list (not a glob) so a Linux build never pulls in the win32/cocoa TUs and a
-- Windows build never pulls in the X11/Wayland ones — GLFW's own CMake makes the same choice.
local root = "%{wks.location}/VAE/vendor/GLFW/"

project "GLFW"
   kind "StaticLib"
   language "C"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   includedirs {
      root .. "include",
      root .. "src",
      "%{wks.location}/VAE/vendor-generated/wayland",
   }

   files {
      root .. "include/GLFW/glfw3.h",
      root .. "include/GLFW/glfw3native.h",
      root .. "src/internal.h",
      root .. "src/platform.h",
      root .. "src/mappings.h",
      root .. "src/context.c",
      root .. "src/init.c",
      root .. "src/input.c",
      root .. "src/monitor.c",
      root .. "src/platform.c",
      root .. "src/vulkan.c",
      root .. "src/window.c",
      root .. "src/egl_context.c",
      root .. "src/osmesa_context.c",
      root .. "src/null_platform.h",
      root .. "src/null_joystick.h",
      root .. "src/null_init.c",
      root .. "src/null_monitor.c",
      root .. "src/null_window.c",
      root .. "src/null_joystick.c",
   }

   filter "system:linux"
      pic "On"
      systemversion "latest"
      -- A fresh checkout must not be able to forget this; the script is idempotent and skips
      -- protocols whose headers are already newer than the XML. Linux-only: there is no Wayland
      -- on Windows and the generated headers are only #included by the wl_* sources.
      prebuildcommands { "@%{wks.location}/scripts/GenWaylandProtocols.sh" }
      files {
         -- X11 backend
         root .. "src/x11_init.c",
         root .. "src/x11_monitor.c",
         root .. "src/x11_window.c",
         root .. "src/xkb_unicode.c",
         root .. "src/glx_context.c",
         root .. "src/posix_module.c",
         root .. "src/posix_poll.c",
         root .. "src/posix_thread.c",
         root .. "src/posix_time.c",
         root .. "src/linux_joystick.c",
         -- Wayland backend. libwayland-client/cursor/egl, libxkbcommon and libdecor are all
         -- dlopened by GLFW at runtime, so none of them is a link-time dependency.
         root .. "src/wl_init.c",
         root .. "src/wl_monitor.c",
         root .. "src/wl_window.c",
         root .. "src/wl_platform.h",
      }
      defines { "_GLFW_X11", "_GLFW_WAYLAND", "_GNU_SOURCE" }

   -- Written, never compiled: the file list GLFW's CMake selects for a Win32 build. See
   -- design/windows.md.
   filter "system:windows"
      systemversion "latest"
      files {
         root .. "src/win32_platform.h",
         root .. "src/win32_joystick.h",
         root .. "src/win32_init.c",
         root .. "src/win32_joystick.c",
         root .. "src/win32_module.c",
         root .. "src/win32_monitor.c",
         root .. "src/win32_thread.c",
         root .. "src/win32_time.c",
         root .. "src/win32_window.c",
         root .. "src/wgl_context.c",
      }
      defines { "_GLFW_WIN32", "_CRT_SECURE_NO_WARNINGS" }

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release or Dist"
      runtime "Release"
      optimize "on"
