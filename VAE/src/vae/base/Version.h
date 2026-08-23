#pragma once

#include <string>

// What this build is. Two numbers, both derived from git by scripts/version.sh and generated into
// vae/Version.gen.h before anything compiles — nothing is declared in a source file, because a
// version in a source file is a version somebody forgets to edit. Releasing is tagging.
//
// Neither number says anything about *compatibility*. That is three other constants, hand-bumped
// on their own schedules: Serializer::kFormatVersion for documents, VAE_SCRIPT_ABI_VERSION for the
// script ABI, and ui::kLibraryVersion for the widget catalog.
#if __has_include(<vae/Version.gen.h>)
#   include <vae/Version.gen.h>
#endif

// A build made without git — a tarball, a shallow clone — says so. Not a plausible-looking number:
// KernelSU's hardcoded fallback of 16 produced "manager 32430, driver 16" reports nobody could
// read, so 0 means "this build does not know what it is" and is printed rather than compared.
#ifndef VAE_VERSION_CODE
#   define VAE_VERSION_CODE 0u
#endif
#ifndef VAE_VERSION_NAME
#   define VAE_VERSION_NAME "unknown"
#endif

namespace vae {

    struct Version {
        // 10000 + commits on HEAD. Monotonic; says which build this is, and nothing else.
        static constexpr unsigned Code() { return VAE_VERSION_CODE; }
        // git describe: "v1.0", "v1.0-12-gabc1234", or a bare hash before the first tag.
        static constexpr const char* Name() { return VAE_VERSION_NAME; }
        static bool Known() { return Code() != 0u; }

        // What a banner, a title bar and a bug report all want: "v1.0-12-gabc1234 (build 10012)".
        static std::string String() {
            if (!Known()) return "unknown build";
            return std::string(Name()) + " (build " + std::to_string(Code()) + ")";
        }
    };

}
