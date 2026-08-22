#pragma once

#include "vae/base/Base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vae::platform {

    // The whole of what VAE asks of the operating system. Four things, and they are here rather
    // than scattered through the code that needs them so that a port is one file to read and one
    // file to fill in — not a search for `#ifdef` across a hundred.
    //
    // Everything else in the engine is standard C++ or a library that has already been ported.
    // Vulkan, GLFW, ImGui, stb, Lua, httplib and miniaudio all run on Windows unchanged; what does
    // not is loading a module, running a compiler, finding the user's folders, and finding fonts.

    // --- dynamic modules -----------------------------------------------------------------------
    // `dlopen`/`LoadLibrary`. The engine loads exactly one kind of module — a compiled component
    // script — and reloads it while the app is running, which is why the copy-before-open dance in
    // NativeHost exists and why it has to keep existing on both systems.
    using Module = void*;

    Module LoadModule(const std::filesystem::path& path);
    void*  ModuleSymbol(Module module, const char* name);
    void   FreeModule(Module module);
    // The last failure, for a message a person can act on. Empty when there was none.
    std::string ModuleError();

    // What a compiled script module is called on this system, with the dot: ".so" or ".dll".
    const char* ModuleExtension();

    // --- running a compiler --------------------------------------------------------------------
    struct Ran {
        int status = -1;
        std::string output;          // stdout and stderr together, in the order they were written
        bool Ok() const { return status == 0; }
    };

    // Runs a command line and captures everything it says. Only ever used to run a C++ compiler,
    // which is why it is one call and not a process API.
    Ran Run(const std::string& command);

    // How a path is spelled inside a command line here. A project called "My app" lives in a folder
    // with a space in it, and a path that is not quoted turns that into two arguments and a
    // compiler error about a file nobody named.
    std::string Quote(const std::filesystem::path& path);

    // The command that turns one .cpp into one loadable module, given the include directory the
    // script header lives in. Different compiler, different flags, same one line of intent.
    std::string CompileCommand(const std::filesystem::path& source,
                               const std::filesystem::path& out,
                               const std::filesystem::path& includeDir);

    // What to tell someone whose machine cannot run `CompileCommand` at all. Different advice on
    // every system, and worth giving: "cl is not recognized" means "you are not in a developer
    // prompt", which is not something a person guesses.
    const char* MissingCompilerHint();

    // Sets a variable in this process's environment, overwriting. Only ever used to point the
    // Vulkan loader at a particular ICD before the loader is initialised, which has to happen in
    // the environment because that is the only interface the loader offers.
    void SetEnv(const char* name, const char* value);

    // --- where the user's things are -------------------------------------------------------------
    std::filesystem::path ExecutablePath();
    std::filesystem::path HomeDirectory();
    // Where a person expects their work to be. Not always "Documents": on Linux it is whatever
    // xdg-user-dirs says, and on Windows it is a known folder rather than a name.
    std::filesystem::path DocumentsDirectory();
    // Per-user configuration and per-user data, which are the same place on Windows and two
    // different places on Linux.
    std::filesystem::path ConfigDirectory();
    std::filesystem::path DataDirectory();

    // Every directory worth scanning for installed fonts, in the order they should win.
    std::vector<std::filesystem::path> FontDirectories();

}
