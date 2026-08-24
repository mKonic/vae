#pragma once

#include "vae/core/Layer.h"
#include "vae/doc/Document.h"

#include <filesystem>
#include <string>

namespace vae {

    // Everything "Export C++" does, in one place, so the menu item and `--export` cannot drift
    // apart. `script` is the file the app will actually load — the .lua, or the built .so for a
    // C++ project, never the .cpp: the runtime picks its host by extension, and handing it a
    // source file is handing it something it will try to run as Lua.
    //
    // `out` empty means "<project folder>/<Name>-export", which is what the menu item uses.
    // Returns false and fills `error` when nothing was written.
    bool ExportProject(const doc::Document& document, const std::filesystem::path& project,
                       const std::filesystem::path& script, std::filesystem::path out,
                       std::string* error);

    // `VAE-Studio --export <project> [directory]` — the same thing with no editor around it. No
    // window and no device: emitting a project is the document layer, the standard library and the
    // file system, all three of which are headless.
    //
    // It exists because "the exported folder builds and runs" is a question a script has to be able
    // to ask. Answering it by driving a menu means the one path that has to work on somebody else's
    // machine is the one path nothing checks.
    class ExportLayer final : public Layer {
    public:
        ExportLayer(std::filesystem::path project, std::filesystem::path out)
            : Layer("Export"), m_Project(std::move(project)), m_Out(std::move(out)) {}
        void OnAttach() override;

    private:
        std::filesystem::path m_Project;
        std::filesystem::path m_Out;
    };

}
