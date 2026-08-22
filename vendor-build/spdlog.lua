-- spdlog compiled as a static lib rather than header-only.
-- Header-only spdlog costs ~1.9s per TU that includes it (measured in Ladle); SPDLOG_COMPILED_LIB
-- moves that into one archive built once. Consumers MUST also define SPDLOG_COMPILED_LIB.
local root = "%{wks.location}/VAE/vendor/spdlog/"

project "spdlog"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files { root .. "src/*.cpp" }
   includedirs { root .. "include" }
   defines { "SPDLOG_COMPILED_LIB" }

   filter "system:linux"
      pic "On"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release or Dist"
      runtime "Release"
      optimize "on"
