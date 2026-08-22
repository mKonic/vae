-- ImGuiColorTextEdit (the goossens rewrite) — the code editor widget behind the Script panel.
-- Two translation units and no dependency beyond Dear ImGui itself, which is why it is vendored
-- rather than reimplemented: colouring, a gutter, multiple cursors and a completion popup are a
-- year of work to get right and none of it is VAE's problem.
local root = "%{wks.location}/VAE/vendor/ImGuiColorTextEdit/"

project "TextEditor"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      root .. "TextEditor.h",
      root .. "TextEditor.cpp",
   }

   includedirs {
      "%{IncludeDir.TextEditor}",
      "%{IncludeDir.ImGui}",
   }

   defines {
      "IMGUI_DEFINE_MATH_OPERATORS",
   }

   filter "system:linux"
      pic "On"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release or Dist"
      runtime "Release"
      optimize "on"
