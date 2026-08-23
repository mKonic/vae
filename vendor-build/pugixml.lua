-- pugixml, compiled as a static lib. One translation unit, no dependencies, MIT.
--
-- Parsing only: format 3 is written by hand (doc/XmlCodec.cpp) because the writer has opinions
-- pugixml's printer does not share — a declared attribute order rather than an alphabetical one,
-- the shorthand spellings, the empty-element form, the text-body rule. Escaping five characters on
-- the way out is not the hard half; reading markup correctly, with a line number when it is wrong,
-- is, and that is the half worth taking off the shelf.
local root = "%{wks.location}/VAE/vendor/pugixml/"

project "pugixml"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files { root .. "src/pugixml.cpp" }
   includedirs { root .. "src" }

   filter "system:linux"
      pic "On"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release or Dist"
      runtime "Release"
      optimize "on"
