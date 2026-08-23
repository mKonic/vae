project "VAE-Studio"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++23"
   staticruntime "off"
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
      "%{IncludeDir.httplib}",
      "%{IncludeDir.GLFW}",
      "%{IncludeDir.ImGui}",
      "%{IncludeDir.TextEditor}",
      "%{IncludeDir.VulkanHeaders}",
   }

   -- https when the host has OpenSSL; plain http and a clear error when not.
   VaeHttpTls()

   -- StaticLib links are NOT transitive under gmake, so every dependency is relisted here.
   links { "VAE", "VAE-Core", "spdlog", "GLFW", "VulkanDeps", "ImGui", "TextEditor", "Lua", "miniaudio", "pugixml" }

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
