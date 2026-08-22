-- VAE (Virtual App Engine) — a visual app builder.
-- Generate: premake5 gmake   (or run ./scripts/Setup.sh)
include "Dependencies.lua"

workspace "VAE"
   architecture "x86_64"
   startproject "VAE-Studio"
   configurations { "Debug", "Release", "Dist" }
   multiprocessorcompile "On"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Vendored dependencies we compile. Header-only deps need no project, just an include dir.
group "Dependencies"
   include "vendor-build/GLFW.lua"
   include "vendor-build/spdlog.lua"
   include "vendor-build/VulkanDeps.lua"
   include "vendor-build/ImGui.lua"
   include "vendor-build/TextEditor.lua"
   include "vendor-build/lua.lua"
   include "vendor-build/miniaudio.lua"
group ""

-- VAE-Core (headless, host-testable) and VAE (full engine) are both declared here.
group "Core"
   include "VAE"
group ""

group "Tools"
   include "VAE-Studio"
   include "VAE-Player"
group ""

group "Tests"
   include "VAE-Tests"
group ""
