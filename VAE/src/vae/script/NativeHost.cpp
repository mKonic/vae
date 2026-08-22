#include "vaepch.h"
#include "vae/script/NativeHost.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/base/Platform.h"

namespace vae::script {

    NativeHost::~NativeHost() { Close(); }

    void NativeHost::Bind(const VaeScriptAPI& api) {
        m_Api = &api;
        if (!m_Handle) return;
        // A freshly mapped module has its own copy of the pointer, so rebinding after a reload is
        // not optional.
        if (auto* fn = reinterpret_cast<VaeScriptRegisterFn>(
                platform::ModuleSymbol(m_Handle, VAE_SCRIPT_REGISTER_SYMBOL)))
            fn(m_Api);
    }

    void NativeHost::Close() {
        platform::FreeModule(m_Handle);
        m_Handle = nullptr;
        m_Classes.clear();

        if (!m_Loaded.empty()) {
            std::error_code ec;
            std::filesystem::remove(m_Loaded, ec);
            m_Loaded.clear();
        }
    }

    void NativeHost::Unload() { Close(); }

    bool NativeHost::Load(const std::filesystem::path& path, std::string* error) {
        Close();
        m_Path = path;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            if (error) *error = "no module at " + path.string();
            return false;
        }

        // Every staged copy this host ever made that is not still mapped. Deleting a loaded module
        // fails silently on Linux and outright on Windows, where a mapped DLL cannot be unlinked at
        // all — so `Close` cannot always clean up after itself and the next load has to.
        const std::string prefix = path.filename().string() + ".";
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), ec)) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(prefix) && name.ends_with(".live"))
                std::filesystem::remove(entry.path(), ec);
        }

        // A private copy per load. Both loaders key their cache on the path, so replacing the file
        // in place and opening it again gives you back the code you were trying to replace.
        //
        // Absolute, because dlopen treats a name with no slash in it as a library to go looking for
        // on the search paths — so a module opened by a relative path is silently not found.
        std::filesystem::path unique = std::filesystem::absolute(path, ec);
        if (ec) unique = path;
        unique += "." + std::to_string(++m_Generation) + ".live";
        std::filesystem::copy_file(path, unique, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) *error = "could not stage the module: " + ec.message();
            return false;
        }

        platform::Module handle = platform::LoadModule(unique);
        if (!handle) {
            if (error) {
                *error = platform::ModuleError();
                if (error->empty()) *error = "could not load " + unique.filename().string();
            }
            std::filesystem::remove(unique, ec);
            return false;
        }

        auto abi = reinterpret_cast<VaeScriptAbiFn>(
            platform::ModuleSymbol(handle, VAE_SCRIPT_ABI_SYMBOL));
        auto reg = reinterpret_cast<VaeScriptRegisterFn>(
            platform::ModuleSymbol(handle, VAE_SCRIPT_REGISTER_SYMBOL));
        auto classes = reinterpret_cast<VaeScriptClassesFn>(
            platform::ModuleSymbol(handle, VAE_SCRIPT_CLASSES_SYMBOL));

        if (!abi || !reg || !classes) {
            if (error)
                *error = path.filename().string() + " is not a VAE script module (missing "
                       + std::string(!abi ? VAE_SCRIPT_ABI_SYMBOL
                                   : !reg ? VAE_SCRIPT_REGISTER_SYMBOL
                                          : VAE_SCRIPT_CLASSES_SYMBOL) + ")";
            platform::FreeModule(handle);
            std::filesystem::remove(unique, ec);
            return false;
        }

        // Refused, not tolerated. A module built against a different table would read the wrong
        // function pointers and crash somewhere unrelated to the mistake.
        const unsigned int version = abi();
        if (version != VAE_SCRIPT_ABI_VERSION) {
            if (error)
                *error = path.filename().string() + " was built against script ABI "
                       + std::to_string(version) + "; this engine speaks "
                       + std::to_string(VAE_SCRIPT_ABI_VERSION);
            platform::FreeModule(handle);
            std::filesystem::remove(unique, ec);
            return false;
        }

        m_Handle = handle;
        m_Loaded = unique;
        if (m_Api) reg(m_Api);

        int count = 0;
        if (const VaeScriptClass* found = classes(&count))
            m_Classes.assign(found, found + std::max(count, 0));

        VAE_CORE_INFO("script: loaded {} ({} class{})", path.filename().string(), m_Classes.size(),
                      m_Classes.size() == 1 ? "" : "es");
        return true;
    }

    bool NativeHost::Reload(std::string* error) {
        if (m_Path.empty()) return true;
        return Load(m_Path, error);
    }

    const VaeScriptClass* NativeHost::Find(std::string_view component) const {
        for (const VaeScriptClass& klass : m_Classes)
            if (klass.component && component == klass.component) return &klass;
        return nullptr;
    }

    std::vector<std::string> NativeHost::Components() const {
        std::vector<std::string> names;
        names.reserve(m_Classes.size());
        for (const VaeScriptClass& klass : m_Classes)
            if (klass.component) names.emplace_back(klass.component);
        return names;
    }

    bool NativeHost::Compile(const std::filesystem::path& source, const std::filesystem::path& out,
                             std::string* diagnostics) {
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);

        // Which compiler, which flags and how a path is spelled on a command line are the system's
        // business, not this file's. What is this file's business is that there is exactly one
        // include path, the engine's source root, and it is only there for VaeScript.h — a script
        // that reaches past it into the engine's own headers is the thing this design prevents.
        const platform::Ran ran = platform::Run(
            platform::CompileCommand(source, out, FileSystem::EngineRoot() / "VAE" / "src"));

        if (ran.Ok()) {
            if (diagnostics) *diagnostics = ran.output;
            return true;
        }

        // A missing compiler is not a compile error, and reporting it as one sends the author
        // looking for a mistake in their script. It is the one failure worth translating.
        std::string output = ran.output;
        for (const std::string_view sign : { "not recognized", "not found", "No such file" }) {
            if (output.find(sign) == std::string::npos) continue;
            output += "\n" + std::string(platform::MissingCompilerHint()) + "\n";
            break;
        }
        if (diagnostics) *diagnostics = output;

        VAE_CORE_ERROR("script: compiling {} failed", source.filename().string());
        return false;
    }

}
