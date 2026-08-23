-- Headless unit tests. Links VAE-Core ONLY — no Vulkan, no GLFW, no ImGui, no window.
-- If a test ever needs one of those, the test is at the wrong level of the pyramid.
project "VAE-Tests"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++23"
   staticruntime "off"
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files { "src/**.h", "src/**.cpp" }

   defines { "SPDLOG_COMPILED_LIB", "GLM_ENABLE_EXPERIMENTAL" }

   includedirs {
      "%{wks.location}/VAE/vendor-generated",
      "src",
      "%{wks.location}/VAE/src",
      "%{IncludeDir.spdlog}",
      "%{IncludeDir.glm}",
      "%{IncludeDir.stb}",
      "%{IncludeDir.json}",
      "%{IncludeDir.pugixml}",
      "%{IncludeDir.httplib}",
   }

   -- https when the host has OpenSSL; plain http and a clear error when not.
   VaeHttpTls()

   links { "VAE-Core", "spdlog", "Lua", "miniaudio", "pugixml" }

   -- The codegen test compiles the emitted builder into a module and dlopens it. That module calls
   -- back into vae::doc::Builder, which lives in this binary, so this binary has to export it.
   linkoptions { "-rdynamic" }

   filter "system:linux"
      systemversion "latest"
      defines { "VAE_PLATFORM_LINUX" }
      -- dl: the script runtime dlopens native component modules, and the suite loads a real one.
      links { "pthread", "dl" }


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
