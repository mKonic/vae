-- VAE (Virtual App Engine) — a visual app builder.
-- Generate: premake5 gmake   (or run ./scripts/Setup.sh)
include "Dependencies.lua"

-- A sanitized build, for finding what a test suite cannot see: reads past the end of a vector,
-- a use-after-free in a cached reference, integer overflow in the layout arithmetic. Not a
-- configuration, because every configuration wants to be sanitizable:
--
--   premake5 gmake --sanitize=address,undefined && make config=debug -j$(nproc)
--   ./bin/Debug-linux-x86_64/VAE-Tests/VAE-Tests
--
-- Then regenerate without the flag to get an unsanitized build back. Sanitized binaries are
-- roughly 2x slower and use far more memory; that is the trade, and it is worth running before a
-- release rather than never.
newoption {
   trigger     = "sanitize",
   value       = "address,undefined",
   description = "Build everything with -fsanitize=<value> (address, undefined, thread, ...)"
}

workspace "VAE"
   architecture "x86_64"
   startproject "VAE-Studio"
   configurations { "Debug", "Release", "Dist" }
   multiprocessorcompile "On"

   if _OPTIONS["sanitize"] then
      local flags = "-fsanitize=" .. _OPTIONS["sanitize"]
      -- Frame pointers so a report names the functions, and symbols so it names the lines.
      buildoptions { flags, "-fno-omit-frame-pointer" }
      linkoptions  { flags }
      symbols "On"
      defines { "VAE_SANITIZE" }
   end

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
   include "vendor-build/pugixml.lua"
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
