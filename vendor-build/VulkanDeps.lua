-- Compiled third-party Vulkan helpers: vk-bootstrap + the VMA implementation TU + stb impls.
-- Separate StaticLib so they build without the engine PCH / warnings.
project "VulkanDeps"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      "%{wks.location}/VAE/vendor/vk-bootstrap/src/VkBootstrap.cpp",
      "%{wks.location}/VAE/vendor/vk-bootstrap/src/VkBootstrap.h",
      "%{wks.location}/vendor-build/src/vk_mem_alloc_impl.cpp",
   }

   includedirs {
      "%{IncludeDir.VulkanHeaders}",
      "%{IncludeDir.VMA}",
      "%{IncludeDir.vkbootstrap}",
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
