-- VAE SDK — everything an app needs to link the engine, in one premake fragment.
--
-- An exported app includes this and calls VaeApp(); nothing else in its premake5.lua is about
-- VAE. That indirection is the point: the link line is long, it changes whenever the engine gains
-- a dependency, and an exported project that spelled it out itself would go stale the first time
-- it did.
--
-- Works against two layouts, because both are real installs:
--
--   clone      sdk/ inside the checkout (or a linked install, whose sdk/ is a symlink to it).
--              Headers come from VAE/src and VAE/vendor, libraries from bin/<config>/.
--   installed  sdk/ under the prefix, written by install.sh --copy. Headers are gathered into
--              sdk/include, libraries into sdk/lib/<config>/.
--
-- The fragment works out which one it is in, so an app's premake file never has to.

local sdk = path.getdirectory(_SCRIPT)
local clone = os.isdir(sdk .. "/../VAE/src")

-- Written by the engine's own premake run: which optional system libraries are compiled into
-- libVAE.a. Read rather than re-detected, because the question is what the engine was built with.
if os.isfile(sdk .. "/config.lua") then include(sdk .. "/config.lua") end

VAE_SDK  = sdk
VAE_ROOT = clone and path.getabsolute(sdk .. "/..") or sdk

-- The engine's own output directory name. Has to be spelled exactly as the engine spells it, or
-- a Release app links Debug libraries and the ODR violations start.
local function vaeoutdir() return "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}" end

function VaeIncludeDirs()
   if clone then
      return {
         VAE_ROOT .. "/VAE/src",
         VAE_ROOT .. "/VAE/vendor-generated",
         VAE_ROOT .. "/VAE/vendor/spdlog/include",
         VAE_ROOT .. "/VAE/vendor/glm",
         VAE_ROOT .. "/VAE/vendor/Vulkan-Headers/include",
      }
   end
   return { VAE_SDK .. "/include" }
end

function VaeLibDirs()
   -- The clone keeps one directory per project; the install flattens them into one.
   if clone then return { VAE_ROOT .. "/bin/" .. vaeoutdir() .. "/*" } end
   return { VAE_SDK .. "/lib/" .. vaeoutdir() }
end

-- Every static library the engine itself links. StaticLib links are NOT transitive under gmake,
-- so a missing one is an undefined reference at the very last step of somebody else's build.
--
-- No ImGui: the editor chrome is installed through a factory only the editor names, so a shipped
-- app neither references it nor links it. That is 1.4 MB of toolkit an exported app used to carry
-- and could never open.
function VaeLinks()
   return { "VAE", "VAE-Core", "spdlog", "GLFW", "VulkanDeps",
            "Lua", "miniaudio", "pugixml", "harfbuzz" }
end

-- Applies everything: language, includes, defines, the link line and the per-system bits.
-- Call it inside a `project` block, after kind/targetdir.
function VaeApp()
   language "C++"
   cppdialect "C++23"

   includedirs (VaeIncludeDirs())

   defines { "SPDLOG_COMPILED_LIB", "GLFW_INCLUDE_NONE",
             "GLM_FORCE_DEPTH_ZERO_TO_ONE", "GLM_ENABLE_EXPERIMENTAL" }

   libdirs (VaeLibDirs())
   links (VaeLinks())

   filter "system:linux"
      defines { "VAE_PLATFORM_LINUX" }
      links { "dl", "pthread", "X11", "vulkan" }
      -- Whatever the engine was built with, the app has to be linked with. httplib is
      -- header-only and the a11y bridge is compiled in, so their calls are already inside
      -- libVAE.a: leaving these out is an undefined reference at the last step of the build.
      if VAE_HAS_OPENSSL then links { "ssl", "crypto" } end
      if VAE_HAS_SDBUS   then links { "systemd" } end

   filter "system:windows"
      systemversion "latest"
      defines { "VAE_PLATFORM_WINDOWS", "NOMINMAX", "WIN32_LEAN_AND_MEAN" }
      buildoptions { "/utf-8" }
      libdirs { (os.getenv("VULKAN_SDK") or "") .. "/Lib" }
      links { "vulkan-1", "gdi32", "ws2_32", "shell32", "ole32" }

   filter {}

   -- The three the engine ships, spelled the same way, because the app's configuration name is
   -- what picks which engine libraries it links.
   filter "configurations:Debug"
      defines { "VAE_DEBUG" }
      symbols "on"

   filter "configurations:Release"
      defines { "VAE_RELEASE" }
      optimize "on"

   filter "configurations:Dist"
      defines { "VAE_DIST" }
      optimize "on"

   filter {}
end
