#pragma once

#include "vae/base/Base.h"

#include <cstdint>
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

    // --- running another program -----------------------------------------------------------------
    //
    // `Run` above waits for what it started and hands back what it said, which is right for a
    // compiler and wrong for an application: the editor launches the player and carries on editing.
    // These three are the other shape — start it, ask whether it is still there, ask it to close.

    // A started process, or 0 when it could not be started. Opaque: a pid here, a handle there.
    using Process = std::uint64_t;

    // Starts `program` with `args` and does not wait. The child gets its own process group, so a
    // Ctrl+C in the terminal the editor was launched from does not take the app down with it.
    Process Launch(const std::filesystem::path& program,
                   const std::vector<std::string>& args = {});

    // Whether it is still running. Reaps it when it is not, so a launcher that polls this does not
    // leave zombies behind.
    bool Running(Process& process);

    // Asks it to close (SIGTERM / WM_CLOSE-equivalent). Not a kill: an app with unsaved work gets
    // to ask about it.
    void AskToClose(Process process);

    // `Process` above is a child *this* program started, and on Windows it is a handle rather than
    // a number. Naming a file after the process that owns it needs the other thing: the number the
    // system uses, which any program can write down and a different one can read back.
    using ProcessId = std::uint32_t;

    ProcessId CurrentProcessId();
    // Whether any process with that id exists. Not whether it is ours, and not whether it is still
    // the same program — an id is reused eventually. Enough to decide whether a file might still
    // be being written to, and not enough for anything else.
    bool ProcessIdAlive(ProcessId id);

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
