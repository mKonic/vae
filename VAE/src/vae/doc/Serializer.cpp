#include "vaepch.h"
#include "vae/doc/Serializer.h"

#include "vae/base/FileSystem.h"
#include "vae/doc/ValueText.h"

#include <nlohmann/json.hpp>

// What is left here is everything that is not a codec: reading and writing the file, choosing which
// codec to hand it to, and the project file. The codecs are JsonCodec.cpp and XmlCodec.cpp.
namespace vae::doc {

    using json = nlohmann::json;
    namespace fs = std::filesystem;


    bool Serializer::Save(const Document& document, const fs::path& path,
                          const LibrarySource* library) {
        return FileSystem::WriteText(path, ToXml(document, true, library));
    }

    bool Serializer::Load(const fs::path& path, Document& out, std::string* error,
                          const LibrarySource* library) {
        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        return FromText(*text, out, error, library);
    }

    // ---------------------------------------------------------------- Project

    bool Project::Save(const Project& project, const fs::path& path) {
        json root;
        root["format"] = "vae.project";
        root["version"] = Serializer::kFormatVersion;
        root["name"] = project.name;
        root["scriptLanguage"] = project.scriptLanguage;
        root["screens"] = project.screens;
        root["components"] = project.components;
        root["fontDirs"] = project.fontDirs;
        root["targetResolution"] = { text::NumberAsDouble(project.targetResolution.x),
                                     text::NumberAsDouble(project.targetResolution.y) };
        return FileSystem::WriteText(path, root.dump(2));
    }

    bool Project::Load(const fs::path& path, Project& out, std::string* error) {
        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        json root = json::parse(*text, nullptr, false);
        if (root.is_discarded() || root.value("format", "") != "vae.project") {
            if (error) *error = "not a VAE project file";
            return false;
        }
        if (root.value("version", 0u) > Serializer::kFormatVersion) {
            if (error) *error = "project was written by a newer VAE";
            return false;
        }

        out.name = root.value("name", "Untitled");
        out.scriptLanguage = root.value("scriptLanguage", "lua");
        out.root = path.parent_path();
        out.screens = root.value("screens", std::vector<std::string>{});
        out.components = root.value("components", std::vector<std::string>{});
        out.fontDirs = root.value("fontDirs", std::vector<std::string>{});
        if (root.contains("targetResolution") && root["targetResolution"].size() == 2)
            out.targetResolution = { root["targetResolution"][0].get<f32>(),
                                     root["targetResolution"][1].get<f32>() };
        return true;
    }

}
