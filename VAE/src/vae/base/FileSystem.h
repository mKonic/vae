#pragma once

#include "vae/base/Base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vae {

    // Resolves engine-owned files (shaders, fonts, default assets) independently of the working
    // directory. Ladle shipped CWD-relative asset paths and the editor only ran from the repo root;
    // that is the bug this class exists to prevent.
    class FileSystem {
    public:
        // Order: $VAE_ROOT, then walking up from the executable looking for the root marker.
        // **Empty for a shipped app**, which is the normal case and not a failure: the engine's
        // shaders are linked into the binary and everything else an app owns lives beside it. Only
        // Studio tooling — the C++ script compiler, the file browser's starting point — reaches
        // for a checkout, and those run out of an install that has one.
        static const std::filesystem::path& EngineRoot();

        // Where the user's own work lives, which is never inside the engine: an installed VAE has
        // no business writing into its own directory, and a checked-out one has no business filling
        // the repository with someone's projects.
        //   $VAE_PROJECTS, then $XDG_DOCUMENTS_DIR/VAE, then ~/Documents/VAE, then
        //   $XDG_DATA_HOME/vae/projects.
        static const std::filesystem::path& ProjectsRoot();
        // Per-user Studio state — recents, and anything else that is a preference rather than work.
        //   $XDG_CONFIG_HOME/vae, then ~/.config/vae.
        static const std::filesystem::path& ConfigRoot();

        // Where the headers and static libraries an exported app compiles against live: the
        // `sdk/` directory beside the engine root, which is the checkout's own in a clone or a
        // linked install, and a gathered copy under the prefix in a copied one. Empty when this
        // build has no SDK beside it, which is what an export has to refuse on rather than emit a
        // project pointing at nothing.
        static std::filesystem::path SdkRoot();

        static std::filesystem::path Asset(std::string_view relative);
        static std::filesystem::path ExecutablePath();
        static std::filesystem::path ExecutableDir();

        static std::optional<std::string>        ReadText(const std::filesystem::path& path);
        static std::optional<std::vector<u8>>    ReadBinary(const std::filesystem::path& path);
        static bool                              WriteText(const std::filesystem::path& path, std::string_view text);

        // Present at the repository/install root; how EngineRoot recognizes where it is.
        static constexpr const char* kRootMarker = ".vaeroot";
    };

}
