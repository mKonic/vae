project "VAE-Studio"
   kind "ConsoleApp"
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

   files { "src/**.h", "src/**.cpp" }

   defines {
      "SPDLOG_COMPILED_LIB",
      "GLFW_INCLUDE_NONE",
      "IMGUI_DEFINE_MATH_OPERATORS",
      "GLM_FORCE_DEPTH_ZERO_TO_ONE",
      "GLM_ENABLE_EXPERIMENTAL",
   }

   includedirs {
      "%{wks.location}/VAE/vendor-generated",
      "src",
      "%{wks.location}/VAE/src",
      "%{IncludeDir.spdlog}",
      "%{IncludeDir.glm}",
      "%{IncludeDir.json}",
      "%{IncludeDir.pugixml}",
      "%{IncludeDir.harfbuzz}",
      "%{IncludeDir.httplib}",
      "%{IncludeDir.GLFW}",
      "%{IncludeDir.ImGui}",
      "%{IncludeDir.TextEditor}",
      "%{IncludeDir.VulkanHeaders}",
   }

   -- https when the host has OpenSSL; plain http and a clear error when not.
   VaeHttpTls()
   VaeAccessibility()

   -- StaticLib links are NOT transitive under gmake, so every dependency is relisted here.
   links { "VAE", "VAE-Core", "spdlog", "GLFW", "VulkanDeps", "ImGui", "TextEditor", "Lua", "miniaudio", "pugixml", "harfbuzz" }

   filter "system:linux"
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
