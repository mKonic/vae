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
   -- First-party code only, never vendor-build/: other people's warnings are noise you cannot act
   -- on, and a wall of them is how a real one goes unread. This codebase compiled at GCC's default
   -- warning level until 2026-08-30, which is close to silent.
   --
   -- -Wmissing-field-initializers is off because it is exactly that noise here: it fires on every
   -- designated initializer that leaves a member to its default, which is the whole point of one,
   -- and it was 469 of the first 487 warnings. Turning it off is what makes the other 18 readable.
   warnings "Extra"
   buildoptions { "-Wno-missing-field-initializers" }
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
      -- What the app looks like to a screen reader. In Core because building the tree is pure
      -- inspection of the view tree, which is what makes it testable without one attached; the
      -- bridge that carries it to the desktop is in the full library.
      "src/vae/a11y/**.h",   "src/vae/a11y/**.cpp",
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

   -- sol2 as a system include: -Wextra fires inside its templates from OUR translation units, and
   -- a warning in a header we do not own is one nobody can act on. externalincludedirs is what
   -- premake spells -isystem.
   externalincludedirs { "%{IncludeDir.sol2}" }

   defines {
      "VAE_ENGINE_BUILD",
      "SPDLOG_COMPILED_LIB",
      "GLM_ENABLE_EXPERIMENTAL",
   }

   -- The build number and the version name, out of git, before anything compiles. Idempotent: the
   -- header is only rewritten when it changed, so this does not recompile the world every build.
   prebuildcommands {
      "@%{wks.location}/scripts/version.sh header %{wks.location}/VAE/vendor-generated/vae/Version.gen.h"
   }

   includedirs {
      "src",
      "vendor-generated",
      "%{IncludeDir.spdlog}",
      "%{IncludeDir.glm}",
      "%{IncludeDir.stb}",
      "%{IncludeDir.json}",
      "%{IncludeDir.pugixml}",
      "%{IncludeDir.harfbuzz}",
      "%{IncludeDir.httplib}",
      "%{IncludeDir.miniaudio}",
      "%{IncludeDir.luashim}",
      "%{IncludeDir.lua}",
   }

   -- https when the host has OpenSSL; plain http and a clear error when not.
   VaeHttpTls()
   VaeAccessibility()

   links { "spdlog", "Lua", "miniaudio", "pugixml", "harfbuzz" }

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
   -- First-party code only, never vendor-build/: other people's warnings are noise you cannot act
   -- on, and a wall of them is how a real one goes unread. This codebase compiled at GCC's default
   -- warning level until 2026-08-30, which is close to silent.
   --
   -- -Wmissing-field-initializers is off because it is exactly that noise here: it fires on every
   -- designated initializer that leaves a member to its default, which is the whole point of one,
   -- and it was 469 of the first 487 warnings. Turning it off is what makes the other 18 readable.
   warnings "Extra"
   buildoptions { "-Wno-missing-field-initializers" }
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   -- The engine's SPIR-V, turned into a header the renderer links into itself, so a binary built
   -- against VAE carries its own pipelines and needs no engine checkout at run time. Reads the
   -- compiled cache, so this needs no glslc; compiling the GLSL is CompileShaders.sh's job and it
   -- calls this at the end. Idempotent, like version.sh above.
   prebuildcommands {
      "@%{wks.location}/scripts/EmbedShaders.sh %{wks.location}/VAE/vendor-generated/vae/Shaders.gen.h"
   }

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
      "vendor-generated",
      "%{IncludeDir.spdlog}",
      "%{IncludeDir.glm}",
      "%{IncludeDir.stb}",
      "%{IncludeDir.json}",
      "%{IncludeDir.pugixml}",
      "%{IncludeDir.harfbuzz}",
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
   VaeAccessibility()

   links { "VAE-Core", "spdlog", "GLFW", "VulkanDeps", "ImGui", "miniaudio", "pugixml", "harfbuzz" }

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
