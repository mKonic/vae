@echo off
REM Compiles every GLSL shader under VAE\assets\shaders to SPIR-V beside itself.
REM glslc ships in the LunarG Vulkan SDK, which the build needs anyway for vulkan-1.lib.
REM
REM The Linux twin is CompileShaders.sh. Written, never run — see design\windows.md.
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "SRC=VAE\assets\shaders"
set "OUT=%SRC%\cache"
if not exist "%OUT%" mkdir "%OUT%"

set FOUND=0
for %%E in (vert frag comp) do (
   for %%F in ("%SRC%\*.%%E") do (
      set FOUND=1
      echo   glslc %%~nxF
      glslc --target-env=vulkan1.3 -O "%%F" -o "%OUT%\%%~nxF.spv" || exit /b 1
   )
)
if "%FOUND%"=="0" echo   (no shaders yet)
exit /b 0
