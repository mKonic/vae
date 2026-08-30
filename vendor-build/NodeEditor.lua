-- imgui-node-editor (the mKonic fork of thedmd's) — the canvas the Graph panel is drawn on.
--
-- Four translation units of node/link hit-testing, navigation, selection and link routing, plus
-- the two from the blueprints example that draw the pin icons — the triangle that means execution
-- and the circle that means a value. Those live under examples/ upstream but are the reference
-- implementation of the look rather than a demo of it.
--
-- `builders.cpp` is deliberately NOT here. It is written against thedmd's fork of Dear ImGui,
-- which carries a stack-layout extension (BeginHorizontal/Spring) that upstream does not have, and
-- VAE vendors upstream. VAE-Studio/src/panels/BlueprintNode.cpp is the replacement: the same
-- drawing, laid out with groups and measured rows.
local root = "%{wks.location}/VAE/vendor/imgui-node-editor/"

project "NodeEditor"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      root .. "imgui_node_editor.h",
      root .. "imgui_node_editor.cpp",
      root .. "imgui_node_editor_api.cpp",
      root .. "imgui_canvas.cpp",
      root .. "crude_json.cpp",
      root .. "examples/blueprints-example/utilities/drawing.cpp",
      root .. "examples/blueprints-example/utilities/widgets.cpp",
   }

   includedirs {
      "%{IncludeDir.NodeEditor}",
      "%{IncludeDir.NodeEditorUtil}",
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
