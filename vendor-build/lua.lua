-- Lua 5.4, built as C. The amalgamated file list is the official one minus the standalone
-- interpreter and the compiler (lua.c / luac.c), which have their own main().
project "Lua"
   kind "StaticLib"
   language "C"
   cdialect "C11"
   staticruntime "off"
   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      "%{wks.location}/VAE/vendor/lua/*.c",
      "%{wks.location}/VAE/vendor/lua/*.h",
   }
   removefiles {
      "%{wks.location}/VAE/vendor/lua/lua.c",
      "%{wks.location}/VAE/vendor/lua/luac.c",
      "%{wks.location}/VAE/vendor/lua/onelua.c",
      "%{wks.location}/VAE/vendor/lua/ltests.c",
   }

   filter "system:linux"
      pic "On"
      systemversion "latest"
      -- LUA_USE_LINUX turns on dlopen-based `require`, readline-free io, and the POSIX bits.
      defines { "LUA_USE_LINUX" }

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      runtime "Release"
      optimize "on"
