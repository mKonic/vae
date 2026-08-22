-- miniaudio: one header, one implementation TU, every desktop audio API.
--
-- The whole reason it is here rather than ALSA directly: ALSA, PulseAudio, WASAPI and CoreAudio
-- behind one interface means the audio service is not a thing the Windows port has to write.
--
-- Vorbis needs stb_vorbis compiled ahead of the implementation, which is what
-- vendor-build/miniaudio_impl.c is for — see the comment at the top of it.
project "miniaudio"
   kind "StaticLib"
   language "C"
   staticruntime "off"
   warnings "off"

   targetdir ("%{wks.location}/bin/"     .. outputdir .. "/%{prj.name}")
   objdir    ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files { "%{wks.location}/vendor-build/miniaudio_impl.c" }

   includedirs { "%{IncludeDir.miniaudio}" }

   filter "system:linux"
      pic "On"
      systemversion "latest"

   filter "system:windows"
      systemversion "latest"
      defines { "_CRT_SECURE_NO_WARNINGS" }

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release or Dist"
      runtime "Release"
      optimize "on"
