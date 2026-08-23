-- HarfBuzz, compiled as a static lib from its own amalgamation. MIT ("Old MIT").
--
-- src/harfbuzz.cc includes every other source file, so the whole library is one translation unit
-- and there is no list of sources here to drift out of date when the submodule is bumped.
--
-- No configuration is defined on purpose. HarfBuzz can be built against FreeType, ICU, Glib,
-- CoreText or DirectWrite for its Unicode data and font access; with none of them it uses its own
-- built-in Unicode tables and its own OpenType font backend, which is what we want — the shaper is
-- the part being taken off the shelf, not a second font stack.
local root = "%{wks.location}/VAE/vendor/harfbuzz/"

project "harfbuzz"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files { root .. "src/harfbuzz.cc" }
   includedirs { root .. "src" }

   defines { "HB_NO_MT" }   -- one thread touches a face; the atomics are pure overhead here

   filter "system:linux"
      pic "On"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"
      -- Even a debug build wants an optimized shaper: unoptimized HarfBuzz is slow enough to show
      -- up as stutter while typing, and nobody debugs into it.
      optimize "on"

   filter "configurations:Release or Dist"
      runtime "Release"
      optimize "on"
