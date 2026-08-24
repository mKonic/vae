#include "Export.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/base/Platform.h"
#include "vae/core/Application.h"
#include "vae/doc/Serializer.h"
#include "vae/gen/Emit.h"
#include "vae/text/FontDB.h"
#include "vae/ui/Library.h"

namespace vae {

    bool ExportProject(const doc::Document& document, const std::filesystem::path& project,
                       const std::filesystem::path& script, std::filesystem::path out,
                       std::string* error) {
        gen::Options options;
        options.appName = project.stem().string();
        if (options.appName.empty()) options.appName = "App";

        std::error_code ec;
        if (!script.empty() && std::filesystem::is_regular_file(script, ec))
            options.script = script.filename().string();
        options.assetRoot = project.parent_path();

        if (out.empty()) out = project.parent_path() / (options.appName + "-export");
        if (!gen::EmitProject(document, out, options, error)) return false;

        // The script goes with it, beside the binary, because that is where the emitted main tells
        // the runtime to look for it.
        if (!options.script.empty()) {
            std::filesystem::copy_file(script, out / options.script,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                if (error) *error = "could not copy " + script.string() + ": " + ec.message();
                return false;
            }
        }

        VAE_INFO("export: {} — run `premake5 gmake && make` in there", out.string());
        return true;
    }

    void ExportLayer::OnAttach() {
        Application& app = Application::Get();
        const auto fail = [&](const std::string& message) {
            VAE_ERROR("export: {}", message);
            app.SetExitCode(1);
            app.Close();
        };

        if (m_Project.empty()) return fail("usage: VAE-Studio --export <project> [directory]");

        doc::Document document;
        std::string error;
        if (!doc::Serializer::Load(m_Project, document, &error, &ui::StandardLibrary()))
            return fail(error);

        // The exporter copies the fonts the *running editor* has registered, so the CLI has to have
        // registered them too. Without this an export from a script carries no fonts at all.
        text::FontDB::Get().LoadDefaults();

        // The script beside the document, named after it — the same rule the editor follows. A
        // built module wins over a source file, because that is what the runtime can load.
        std::filesystem::path base = m_Project;
        base.replace_extension();
        std::filesystem::path script = base;
        script += platform::ModuleExtension();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(script, ec)) {
            script = base;
            script += ".lua";
            if (!std::filesystem::is_regular_file(script, ec)) script.clear();
        }

        if (!ExportProject(document, m_Project, script, m_Out, &error)) return fail(error);
        app.Close();
    }

}
