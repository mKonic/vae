#include "vaepch.h"
#include "vae/base/FileSystem.h"

#include "vae/base/Platform.h"

#include <cstdlib>
#include <fstream>

namespace vae {

    namespace fs = std::filesystem;

    fs::path FileSystem::ExecutablePath() { return platform::ExecutablePath(); }

    fs::path FileSystem::ExecutableDir() { return ExecutablePath().parent_path(); }

    const fs::path& FileSystem::EngineRoot() {
        static const fs::path root = [] () -> fs::path {
            std::error_code ec;

            if (const char* env = std::getenv("VAE_ROOT")) {
                fs::path p(env);
                if (fs::exists(p, ec)) return p;
                VAE_CORE_WARN("VAE_ROOT points at '{}', which does not exist — ignoring", env);
            }

            for (fs::path dir = ExecutableDir(); !dir.empty(); dir = dir.parent_path()) {
                if (fs::exists(dir / kRootMarker, ec)) return dir;
                if (dir == dir.parent_path()) break;   // hit "/"
            }

            // Not a warning: a shipped app has no engine root and does not need one. Shaders are
            // compiled into the library and fonts, pictures and translations live beside the
            // binary. Only the editor's own tooling reaches for the checkout, and that is an
            // install that has one.
            return {};
        }();
        return root;
    }

    const fs::path& FileSystem::ProjectsRoot() {
        static const fs::path root = [] () -> fs::path {
            std::error_code ec;
            if (const char* env = std::getenv("VAE_PROJECTS"); env && *env) {
                fs::path p(env);
                fs::create_directories(p, ec);
                return p;
            }

            const fs::path documents = platform::DocumentsDirectory();
            if (fs::is_directory(documents, ec)) {
                const fs::path p = documents / "VAE";
                fs::create_directories(p, ec);
                return p;
            }

            // No Documents at all — a server, a container, a stripped home. Work still has to go
            // somewhere that is not the engine.
            const fs::path p = platform::DataDirectory() / "vae" / "projects";
            fs::create_directories(p, ec);
            return p;
        }();
        return root;
    }

    const fs::path& FileSystem::ConfigRoot() {
        static const fs::path root = [] {
            std::error_code ec;
            const fs::path p = platform::ConfigDirectory() / "vae";
            fs::create_directories(p, ec);
            return p;
        }();
        return root;
    }

    fs::path FileSystem::SdkRoot() {
        std::error_code ec;
        if (const char* env = std::getenv("VAE_SDK"); env && *env) {
            const fs::path p(env);
            if (fs::exists(p / "vae.lua", ec)) return p;
            VAE_CORE_WARN("VAE_SDK points at '{}', which holds no vae.lua — ignoring", env);
        }
        const fs::path sdk = EngineRoot() / "sdk";
        return fs::exists(sdk / "vae.lua", ec) ? sdk : fs::path{};
    }

    // Empty when there is no engine root, rather than a path relative to whatever directory the
    // app happens to have been started in. A shipped app that looked up "VAE/assets/fonts" from
    // the working directory would find one machine's checkout and not another's.
    fs::path FileSystem::Asset(std::string_view relative) {
        return EngineRoot().empty() ? fs::path{} : EngineRoot() / relative;
    }

    std::optional<std::string> FileSystem::ReadText(const fs::path& path) {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) return std::nullopt;
        std::string out;
        in.seekg(0, std::ios::end);
        out.resize(static_cast<std::size_t>(in.tellg()));
        in.seekg(0, std::ios::beg);
        in.read(out.data(), static_cast<std::streamsize>(out.size()));
        return out;
    }

    std::optional<std::vector<u8>> FileSystem::ReadBinary(const fs::path& path) {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) return std::nullopt;
        in.seekg(0, std::ios::end);
        std::vector<u8> out(static_cast<std::size_t>(in.tellg()));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return out;
    }

    bool FileSystem::WriteText(const fs::path& path, std::string_view text) {
        std::error_code ec;
        if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return out.good();
    }

}
