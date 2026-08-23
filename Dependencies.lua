-- VAE dependency include-dir + library tables.
-- All vendored deps live under VAE/vendor as git submodules.

IncludeDir = {}
IncludeDir["GLFW"]          = "%{wks.location}/VAE/vendor/GLFW/include"
IncludeDir["ImGui"]         = "%{wks.location}/VAE/vendor/imgui"
IncludeDir["TextEditor"]    = "%{wks.location}/VAE/vendor/ImGuiColorTextEdit"
IncludeDir["httplib"]       = "%{wks.location}/VAE/vendor/httplib"
IncludeDir["glm"]           = "%{wks.location}/VAE/vendor/glm"
IncludeDir["spdlog"]        = "%{wks.location}/VAE/vendor/spdlog/include"
IncludeDir["stb"]           = "%{wks.location}/VAE/vendor/stb"
IncludeDir["miniaudio"]     = "%{wks.location}/VAE/vendor/miniaudio"
IncludeDir["json"]          = "%{wks.location}/VAE/vendor/json/single_include"
IncludeDir["pugixml"]       = "%{wks.location}/VAE/vendor/pugixml/src"
IncludeDir["harfbuzz"]      = "%{wks.location}/VAE/vendor/harfbuzz/src"
IncludeDir["VulkanHeaders"] = "%{wks.location}/VAE/vendor/Vulkan-Headers/include"
IncludeDir["VMA"]           = "%{wks.location}/VAE/vendor/VulkanMemoryAllocator/include"
IncludeDir["vkbootstrap"]   = "%{wks.location}/VAE/vendor/vk-bootstrap/src"
-- lua-shim must come BEFORE lua on the include path: it holds the lua.hpp the git mirror omits,
-- and putting it first is what stops sol2 finding the system's Lua headers instead.
IncludeDir["luashim"]       = "%{wks.location}/VAE/vendor/lua-shim"
IncludeDir["lua"]           = "%{wks.location}/VAE/vendor/lua"
IncludeDir["sol2"]          = "%{wks.location}/VAE/vendor/sol2/include"

-- Vulkan loader comes from the system. On Linux that is vulkan-icd-loader, already on the link
-- path; on Windows it is vulkan-1.lib, which ships in the LunarG SDK and is found through the
-- VULKAN_SDK environment variable the installer sets. Headers are vendored above either way, so a
-- Linux build needs no SDK at all and a Windows build needs it only for that one .lib.
-- `or ""` because this line runs on every system: premake reads the whole script before it
-- decides which filters apply, and `nil .. "/Lib"` is an error even in a block that will
-- never be used.
VULKAN_SDK = os.getenv("VULKAN_SDK") or ""

LibraryDir = {}
LibraryDir["VulkanSDK"] = "%{VULKAN_SDK}/Lib"

Library = {}
Library["Vulkan"] = "vulkan"          -- Linux loader
Library["VulkanWindows"] = "vulkan-1" -- Windows loader, from the LunarG SDK

-- HTTPS needs OpenSSL. It is not vendored: every desktop already has it, and shipping a second copy
-- of a TLS stack is how a project ends up with an unpatched one. When it is missing the engine still
-- builds and https requests fail with a message that says exactly this.
function VaeHasOpenSSL()
   if os.target() == "windows" then return false end
   return os.isfile("/usr/include/openssl/ssl.h")
       or os.isfile("/usr/local/include/openssl/ssl.h")
end

-- The screen-reader bridge speaks D-Bus, and sd-bus is the D-Bus client every systemd desktop
-- already has. Not vendored, for the same reason OpenSSL is not: a second copy of a bus client is
-- a second thing to keep in step with the bus. Without it the engine still builds and an app
-- simply has no bridge — which is what happens on a machine with no accessibility bus anyway.
function VaeHasSdBus()
   if os.target() ~= "linux" then return false end
   return os.isfile("/usr/include/systemd/sd-bus.h")
       or os.isfile("/usr/local/include/systemd/sd-bus.h")
end

function VaeAccessibility()
   if not VaeHasSdBus() then return end
   defines { "VAE_A11Y_ATSPI" }
   filter "system:linux"
      links { "systemd" }
   filter {}
end

-- Applied by every project that compiles or links the services layer.
function VaeHttpTls()
   if not VaeHasOpenSSL() then return end
   -- Both, and from here rather than from a #define in one .cpp: httplib is header-only, and a
   -- translation unit that sees it configured differently from its neighbour gets different struct
   -- layouts and different inline bodies. That is an ODR violation, and it presents as a crash
   -- somewhere inside the library with a backtrace that explains nothing.
   defines { "VAE_HTTP_TLS", "CPPHTTPLIB_OPENSSL_SUPPORT" }
   filter "system:linux"
      links { "ssl", "crypto" }
   filter {}
end

-- Everything Windows needs and Linux does not, in one place rather than copied into five project
-- files. Applied by every project that compiles engine code.
--
-- Written, never compiled — see design/windows.md, which says which lines are known and which are
-- the documented-equivalent guess.
function VaeWindowsCommon()
   filter "system:windows"
      systemversion "latest"
      defines {
         "VAE_PLATFORM_WINDOWS",
         "NOMINMAX",              -- or windows.h's min/max macros break <algorithm> and glm
         "WIN32_LEAN_AND_MEAN",
         "_CRT_SECURE_NO_WARNINGS",
      }
      -- Not optional: the source is UTF-8 and full of em-dashes and box characters, and MSVC
      -- without this reads them in the system codepage. The result is mojibake in every message
      -- the engine prints and, in a few places, a C2001 on a "newline in constant".
      buildoptions { "/utf-8" }
      -- ws2_32: sockets. shell32 + ole32: SHGetKnownFolderPath and CoTaskMemFree, which is how
      -- platform.cpp finds the user's folders.
      links { "ws2_32", "shell32", "ole32" }
   filter {}
end

-- The same for anything that opens a window and talks to a GPU.
function VaeWindowsGraphics()
   filter "system:windows"
      libdirs { "%{LibraryDir.VulkanSDK}" }
      links { "%{Library.VulkanWindows}", "gdi32" }
   filter {}
end
