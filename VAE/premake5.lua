-- The engine is TWO static libraries, and the split is load-bearing:
--
--   VAE-Core  pure, headless, host-testable. Document model, layout solver, reactive graph,
--             text measurement, codegen. Links no Vulkan, no GLFW, no ImGui — so VAE-Tests
--             physically CANNOT drift into needing a GPU or a window.
--   VAE       the full engine: window, input, GPU backends, renderer.
--             Links VAE-Core.
--
-- Both share one source tree and one PCH so includes read the same everywhere
-- (#include "vae/doc/Document.h"). vaepch.h must therefore stay free of Vulkan/GLFW/ImGui.

project "VAE-Core"
   kind "StaticLib"
   language "C++"
   cppdialect "C++23"
   staticruntime "off"
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   pchheader "vaepch.h"
   pchsource "src/vaepch.cpp"

   files {
      "src/vaepch.h",
      "src/vaepch.cpp",
      "src/vae/base/**.h",   "src/vae/base/**.cpp",
      "src/vae/rx/**.h",     "src/vae/rx/**.cpp",
      "src/vae/doc/**.h",    "src/vae/doc/**.cpp",
      "src/vae/layout/**.h", "src/vae/layout/**.cpp",
      -- Motion is arithmetic over time: curves, a spring integrator, and a table of animations in
      -- flight. No device, no window, and determinism under a fixed timestep is exactly the kind of
      -- thing a headless test can pin down and a screenshot cannot.
      "src/vae/motion/**.h",  "src/vae/motion/**.cpp",
      -- Services are what an app reaches outside itself: files, saved state, the network, the
      -- clock. All of it is testable against a local fixture, and none of it needs a device.
      "src/vae/svc/**.h",     "src/vae/svc/**.cpp",
      "src/vae/text/**.h",   "src/vae/text/**.cpp",
      -- Vector art is geometry and a scanline rasterizer: paths, an SVG reader, and coverage
      -- written into a buffer. All of it is arithmetic the headless suite can check exactly,
      -- which is the only way to know an icon came out the right shape.
      "src/vae/vector/**.h", "src/vae/vector/**.cpp",
      -- Widgets are pure logic over the document, the layout solver and the draw list. Keeping
      -- them here is what makes every interaction contract testable without a window: VAE-Tests
      -- drives a real button through a real hit-test and never opens a device.
      "src/vae/ui/**.h",     "src/vae/ui/**.cpp",
      -- Events are POD and KeyCodes are constants; neither pulls in a window system, and the
      -- widget layer needs both.
      "src/vae/core/Event.h", "src/vae/core/KeyCodes.h",
      "src/vae/gen/**.h",    "src/vae/gen/**.cpp",
      -- Scripting is logic over the document and the widget host: dlopen and Lua, no device and
      -- no window. Keeping it here is what lets the headless suite run a real component script.
      "src/vae/script/**.h", "src/vae/script/**.cpp",
      -- The draw layer's *recording* half is pure CPU logic (batching, clip intersection,
      -- transform composition) and belongs where the headless tests can reach it. Only
      -- Renderer.cpp, which talks to a device, stays in the full engine.
      "src/vae/draw/Paint.h",
      "src/vae/draw/DrawList.h", "src/vae/draw/DrawList.cpp",
   }

   defines {
      "VAE_ENGINE_BUILD",
      "SPDLOG_COMPILED_LIB",
      "GLM_ENABLE_EXPERIMENTAL",
   }

   includedirs {
      "src",
      "%{IncludeDir.spdlog}",
      "%{IncludeDir.glm}",
      "%{IncludeDir.stb}",
      "%{IncludeDir.json}",
      "%{IncludeDir.pugixml}",
      "%{IncludeDir.httplib}",
      "%{IncludeDir.miniaudio}",
      "%{IncludeDir.luashim}",
      "%{IncludeDir.lua}",
      "%{IncludeDir.sol2}",
   }

   -- https when the host has OpenSSL; plain http and a clear error when not.
   VaeHttpTls()

   links { "spdlog", "Lua", "miniaudio", "pugixml" }

   filter "system:linux"
      pic "On"
      systemversion "latest"
      defines { "VAE_PLATFORM_LINUX" }

   filter {}
   VaeWindowsCommon()

   filter "configurations:Debug"
      defines { "VAE_DEBUG" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "VAE_RELEASE" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "VAE_DIST" }
      runtime "Release"
      optimize "on"


project "VAE"
   kind "StaticLib"
   language "C++"
   cppdialect "C++23"
   staticruntime "off"
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   pchheader "vaepch.h"
   pchsource "src/vaepch.cpp"

   files {
      "src/vaepch.h",
      "src/vaepch.cpp",
      "src/vae/core/**.h",   "src/vae/core/**.cpp",
      "src/vae/gpu/**.h",    "src/vae/gpu/**.cpp",
      "src/vae/draw/Renderer.h", "src/vae/draw/Renderer.cpp",
      "src/vae/app/**.h",    "src/vae/app/**.cpp",
      "src/vae/svc/**.h",    "src/vae/svc/**.cpp",
      "src/platform/**.h",   "src/platform/**.cpp",
   }

   defines {
      "VAE_ENGINE_BUILD",
      "SPDLOG_COMPILED_LIB",
      "GLFW_INCLUDE_NONE",           -- we include the Vulkan headers ourselves
      "IMGUI_DEFINE_MATH_OPERATORS",
      "GLM_FORCE_DEPTH_ZERO_TO_ONE", -- Vulkan clip-space depth
      "GLM_ENABLE_EXPERIMENTAL",
   }

   includedirs {
      "src",
      "%{IncludeDir.spdlog}",
      "%{IncludeDir.glm}",
      "%{IncludeDir.stb}",
      "%{IncludeDir.json}",
      "%{IncludeDir.pugixml}",
      "%{IncludeDir.httplib}",
      "%{IncludeDir.miniaudio}",
      "%{IncludeDir.GLFW}",
      "%{IncludeDir.ImGui}",
      "%{IncludeDir.VulkanHeaders}",
      "%{IncludeDir.VMA}",
      "%{IncludeDir.vkbootstrap}",
   }

   -- https when the host has OpenSSL; plain http and a clear error when not.
   VaeHttpTls()

   links { "VAE-Core", "spdlog", "GLFW", "VulkanDeps", "ImGui", "miniaudio", "pugixml" }

   filter "system:linux"
      pic "On"
      systemversion "latest"
      defines { "VAE_PLATFORM_LINUX" }
      links { "dl", "pthread", "X11", "%{Library.Vulkan}" }

   filter {}
   VaeWindowsCommon()
   VaeWindowsGraphics()

   filter "configurations:Debug"
      defines { "VAE_DEBUG" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "VAE_RELEASE" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "VAE_DIST" }
      runtime "Release"
      optimize "on"
