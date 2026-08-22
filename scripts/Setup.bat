@echo off
REM Initialises submodules and generates the Visual Studio solution. Run from anywhere.
REM
REM The Linux twin is Setup.sh. Written, never run — see design\windows.md.
setlocal
cd /d "%~dp0.."

echo ==^> submodules
git submodule update --init --recursive || goto :failed

echo ==^> shaders
call "%~dp0CompileShaders.bat" || goto :failed

echo ==^> premake
REM premake5 5.0.0-beta8. vs2022 rather than gmake: MSVC is the compiler the script host
REM (NativeHost) shells out to, and a project built with mingw cannot load a module built with cl.
premake5 vs2022 || goto :failed

echo ==^> done. open VAE.sln, or:  msbuild VAE.sln /p:Configuration=Debug
exit /b 0

:failed
echo failed.
exit /b 1
