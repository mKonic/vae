#pragma once

#include "vae/doc/Document.h"

#include <filesystem>
#include <string>

namespace vae::doc {

    // JSON, not YAML. These files are machine-written and can get large; JSON parses faster, diffs
    // cleanly in git, and keeps a future Figma import from needing a second format.
    class Serializer {
    public:
        // Bumped when the on-disk shape changes in a way an older reader could misread. Documents
        // written by a NEWER version are refused rather than half-read.
        static constexpr u32 kFormatVersion = 1;

        static std::string ToJson(const Document& document, bool pretty = true);
        static bool FromJson(std::string_view json, Document& out, std::string* error = nullptr);

        static bool Save(const Document& document, const std::filesystem::path& path);
        static bool Load(const std::filesystem::path& path, Document& out, std::string* error = nullptr);
    };

    // The project file: which screens exist, where assets live, which scripting language, and the
    // font families the project registers.
    struct Project {
        std::string name = "Untitled";
        std::string scriptLanguage = "lua";     // "lua" or "cpp", chosen once at creation
        std::filesystem::path root;
        std::vector<std::string> screens;       // relative paths to .vaescreen files
        std::vector<std::string> components;    // relative paths to .vaecomp files
        std::vector<std::string> fontDirs;
        Vec2 targetResolution{ 1280.0f, 800.0f };

        static bool Save(const Project& project, const std::filesystem::path& path);
        static bool Load(const std::filesystem::path& path, Project& out, std::string* error = nullptr);
    };

}
