-- Dear ImGui (docking branch) + GLFW/Vulkan backends, built as a static lib.
local root = "%{wks.location}/VAE/vendor/imgui/"

project "ImGui"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      root .. "imgui.h",
      root .. "imgui.cpp",
      root .. "imgui_internal.h",
      root .. "imgui_draw.cpp",
      root .. "imgui_tables.cpp",
      root .. "imgui_widgets.cpp",
      root .. "imgui_demo.cpp",
      root .. "misc/cpp/imgui_stdlib.h",
      root .. "misc/cpp/imgui_stdlib.cpp",
      root .. "backends/imgui_impl_glfw.h",
      root .. "backends/imgui_impl_glfw.cpp",
      root .. "backends/imgui_impl_vulkan.h",
      root .. "backends/imgui_impl_vulkan.cpp",
   }

   includedirs {
      root,
      "%{IncludeDir.GLFW}",
      "%{IncludeDir.VulkanHeaders}",
   }

   defines {
      "GLFW_INCLUDE_NONE",
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
