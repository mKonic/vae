#include "vaepch.h"
#include "vae/base/Platform.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>

#ifdef VAE_PLATFORM_LINUX
    #include <dlfcn.h>
    #include <limits.h>
    #include <unistd.h>
#endif

#ifdef VAE_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shlobj.h>
#endif

namespace vae::platform {

    namespace fs = std::filesystem;

#ifdef VAE_PLATFORM_LINUX

    Module LoadModule(const fs::path& path) {
        ::dlerror();
        // RTLD_LOCAL, because two loaded modules must not see each other's symbols: a script that
        // happens to name a function the same as another script's is not a link error waiting to
        // happen, it is two scripts.
        return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    }

    void* ModuleSymbol(Module module, const char* name) { return ::dlsym(module, name); }
    void  FreeModule(Module module) { if (module) ::dlclose(module); }

    std::string ModuleError() {
        const char* text = ::dlerror();
        return text ? text : std::string{};
    }

    const char* ModuleExtension() { return ".so"; }

    Ran Run(const std::string& command) {
        Ran ran;
        // Redirected together, because a compiler writes errors to one and notes to the other and
        // reading them out of order is worse than reading them interleaved.
        FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
        if (!pipe) {
            ran.output = "could not start: " + command;
            return ran;
        }
        std::array<char, 512> buffer{};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            ran.output += buffer.data();
        ran.status = ::pclose(pipe);
        return ran;
    }

    std::string Quote(const fs::path& path) {
        std::string quoted = "'";
        for (const char c : path.string()) {
            if (c == '\'') quoted += "'\\''";
            else            quoted += c;
        }
        return quoted + "'";
    }

    std::string CompileCommand(const fs::path& source, const fs::path& out,
                               const fs::path& includeDir) {
        // One include path, and it is only there for VaeScript.h. A script that reaches past it
        // into the engine's own headers is the thing this design exists to prevent.
        return "g++ -std=c++23 -shared -fPIC -O2 -g -fvisibility=hidden -I" + Quote(includeDir)
             + " " + Quote(source) + " -o " + Quote(out);
    }

    const char* MissingCompilerHint() {
        return "C++ scripts need a compiler: install g++ (build-essential, base-devel, gcc-c++).";
    }

    void SetEnv(const char* name, const char* value) { ::setenv(name, value, 1); }

    fs::path ExecutablePath() {
        char buffer[PATH_MAX];
        const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length > 0) { buffer[length] = '\0'; return fs::path(buffer); }
        return fs::current_path();
    }

    fs::path HomeDirectory() {
        if (const char* home = std::getenv("HOME")) return fs::path(home);
        return fs::current_path();
    }

    fs::path DocumentsDirectory() {
        if (const char* env = std::getenv("XDG_DOCUMENTS_DIR"); env && *env) return fs::path(env);

        // The one line of xdg-user-dirs worth reading: DOCUMENTS is where people expect their work
        // to be, and it is not called "Documents" in every locale.
        const char* config = std::getenv("XDG_CONFIG_HOME");
        const fs::path file = config && *config ? fs::path(config) / "user-dirs.dirs"
                                                : HomeDirectory() / ".config" / "user-dirs.dirs";
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            constexpr std::string_view key = "XDG_DOCUMENTS_DIR=";
            const std::size_t at = line.find(key);
            if (at == std::string::npos) continue;
            std::string value = line.substr(at + key.size());
            if (!value.empty() && value.front() == '"') value.erase(0, 1);
            if (!value.empty() && value.back() == '"') value.pop_back();
            if (value.rfind("$HOME", 0) == 0) return HomeDirectory() / value.substr(6);
            if (!value.empty()) return fs::path(value);
        }
        return HomeDirectory() / "Documents";
    }

    fs::path ConfigDirectory() {
        if (const char* env = std::getenv("XDG_CONFIG_HOME"); env && *env) return fs::path(env);
        return HomeDirectory() / ".config";
    }

    fs::path DataDirectory() {
        if (const char* env = std::getenv("XDG_DATA_HOME"); env && *env) return fs::path(env);
        return HomeDirectory() / ".local" / "share";
    }

    std::vector<fs::path> FontDirectories() {
        const fs::path home = HomeDirectory();
        return { "/usr/share/fonts", "/usr/local/share/fonts",
                 home / ".local" / "share" / "fonts", home / ".fonts" };
    }

#elif defined(VAE_PLATFORM_WINDOWS)

    // ---------------------------------------------------------------------------------------------
    // Written, never compiled. This half exists so the port is a session of fixing what the
    // compiler says rather than a session of deciding what the shape should be — every one of these
    // is the documented Win32 equivalent of the call above it, and every one of them wants checking
    // against a real build. `design/windows.md` says which are guesses.

    Module LoadModule(const fs::path& path) {
        return ::LoadLibraryW(path.wstring().c_str());
    }

    void* ModuleSymbol(Module module, const char* name) {
        return reinterpret_cast<void*>(
            ::GetProcAddress(static_cast<HMODULE>(module), name));
    }

    void FreeModule(Module module) {
        if (module) ::FreeLibrary(static_cast<HMODULE>(module));
    }

    std::string ModuleError() {
        const DWORD code = ::GetLastError();
        if (code == 0) return {};
        char* text = nullptr;
        const DWORD length = ::FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
        std::string message = length && text ? std::string(text, length) : "error " + std::to_string(code);
        if (text) ::LocalFree(text);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
            message.pop_back();
        return message;
    }

    const char* ModuleExtension() { return ".dll"; }

    Ran Run(const std::string& command) {
        Ran ran;
        FILE* pipe = ::_popen((command + " 2>&1").c_str(), "r");
        if (!pipe) {
            ran.output = "could not start: " + command;
            return ran;
        }
        std::array<char, 512> buffer{};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            ran.output += buffer.data();
        ran.status = ::_pclose(pipe);
        return ran;
    }

    std::string Quote(const fs::path& path) {
        // cmd.exe quoting, which is not shell quoting: a double quote is the only delimiter, and a
        // path cannot contain one. Backslashes need no escaping outside a quoted string's tail.
        return "\"" + path.string() + "\"";
    }

    std::string CompileCommand(const fs::path& source, const fs::path& out,
                               const fs::path& includeDir) {
        // MSVC, and it wants a developer environment: `cl` is not on PATH unless the caller came
        // from a Developer Command Prompt or ran vcvarsall.bat. That is the first thing the port
        // has to decide — find the toolchain, or refuse with an instruction.
        return "cl /nologo /std:c++latest /LD /O2 /EHsc /I" + Quote(includeDir) + " "
             + Quote(source) + " /link /OUT:" + Quote(out);
    }

    const char* MissingCompilerHint() {
        return "C++ scripts need MSVC, and `cl` is only on PATH inside a Developer Command Prompt. "
               "Start VAE from one, or run vcvars64.bat first.";
    }

    void SetEnv(const char* name, const char* value) { ::_putenv_s(name, value); }

    fs::path ExecutablePath() {
        wchar_t buffer[MAX_PATH];
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return length ? fs::path(std::wstring(buffer, length)) : fs::current_path();
    }

    namespace {
        fs::path KnownFolder(REFKNOWNFOLDERID id, const fs::path& fallback) {
            PWSTR path = nullptr;
            if (::SHGetKnownFolderPath(id, 0, nullptr, &path) == S_OK && path) {
                fs::path result(path);
                ::CoTaskMemFree(path);
                return result;
            }
            if (path) ::CoTaskMemFree(path);
            return fallback;
        }
    }

    fs::path HomeDirectory() { return KnownFolder(FOLDERID_Profile, fs::current_path()); }
    fs::path DocumentsDirectory() {
        return KnownFolder(FOLDERID_Documents, HomeDirectory() / "Documents");
    }
    // One place for both, unlike Linux: Windows does not separate configuration from data, and
    // inventing a split here would put a project's settings somewhere nobody would look for them.
    fs::path ConfigDirectory() {
        return KnownFolder(FOLDERID_RoamingAppData, HomeDirectory() / "AppData" / "Roaming");
    }
    fs::path DataDirectory() {
        return KnownFolder(FOLDERID_LocalAppData, HomeDirectory() / "AppData" / "Local");
    }

    std::vector<fs::path> FontDirectories() {
        return { KnownFolder(FOLDERID_Fonts, "C:/Windows/Fonts"),
                 DataDirectory() / "Microsoft" / "Windows" / "Fonts" };
    }

#else
    #error "vae::platform has no implementation for this system — see design/windows.md"
#endif

}
