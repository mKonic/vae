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

    bool Serializer::LoadInto(const fs::path& path, Document& out, std::string* error,
                              const LibrarySource* library) {
        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        std::size_t i = 0;
        while (i < text->size() && std::isspace(static_cast<unsigned char>((*text)[i]))) ++i;
        if (i >= text->size() || (*text)[i] != '<') {
            // The older JSON formats describe a whole document, so there is no sensible way to
            // merge one into another. A split project is written by this build and is always
            // format 3.
            if (error) *error = path.filename().string() + " is not a format 3 document";
            return false;
        }
        return FromXml(*text, out, error, library, true);
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

    // ------------------------------------------------- a project split across files

    namespace {

        // A file name for a screen or component, from its own name. Not the node's id: the point of
        // splitting a project up is that a person can tell from `git status` which screen changed.
        std::string FileStem(std::string_view name, std::string_view fallback) {
            std::string stem;
            for (char c : name) {
                const auto u = static_cast<unsigned char>(c);
                if (std::isalnum(u) || c == '-' || c == '_') stem.push_back(c);
                else if (c == ' ' && !stem.empty() && stem.back() != ' ') stem.push_back(' ');
            }
            while (!stem.empty() && stem.back() == ' ') stem.pop_back();
            return stem.empty() ? std::string(fallback) : stem;
        }

        // A document holding the catalog and exactly one of `from`'s roots. That is what one file
        // of a split project is — the writer already leaves out everything the catalog builds, so
        // what lands on disk is the screen and nothing else.
        bool WriteRoot(const Document& from, Uuid root, const fs::path& path,
                       const LibrarySource* library) {
            Document part;
            if (library && !library->Install(library->Id(), library->Version(), part)) return false;
            part.SetTheme(from.ActiveTheme());
            CopySubtreeInto(from, root, part, Uuid::Invalid(), false);
            // A screen that is the project's entry point says so in its own file, so the answer
            // survives without the index having to hold a copy of it.
            if (from.StartScreen() == root) part.SetStartScreen(root);
            return Serializer::Save(part, path, library);
        }

        // Files this project used to have and no longer does. Left behind, a deleted screen still
        // loads on the next open.
        void RemoveOrphans(const fs::path& dir, const std::vector<std::string>& keep,
                           std::string_view extension) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.path().extension() != extension) continue;
                const std::string relative = dir.filename().string() + "/"
                                           + entry.path().filename().string();
                if (std::find(keep.begin(), keep.end(), relative) == keep.end())
                    fs::remove(entry.path(), ec);
            }
        }

    }

    bool Project::IsProjectFile(const fs::path& path) { return path.extension() == ".vaeproj"; }

    bool Project::SaveDocument(const Document& document, Project& project,
                               const fs::path& projectFile, const LibrarySource* library) {
        const fs::path root = projectFile.parent_path();
        std::error_code ec;
        fs::create_directories(root / "screens", ec);
        fs::create_directories(root / "components", ec);

        // A component the catalog still builds identically is a reference, not content: it is not
        // in any file today and it does not get one now.
        std::vector<Uuid> stock;
        if (library) stock = library->Stock(document);
        const auto isStock = [&](Uuid id) {
            return std::find(stock.begin(), stock.end(), id) != stock.end();
        };

        project.screens.clear();
        project.components.clear();
        u32 unnamed = 0;

        for (Uuid id : document.Roots()) {
            const Node* node = document.Find(id);
            if (!node) continue;

            if (node->kind == NodeKind::Screen) {
                const std::string relative = "screens/"
                    + FileStem(node->name, "Screen " + std::to_string(++unnamed)) + ".vaescreen";
                if (!WriteRoot(document, id, root / relative, library)) return false;
                project.screens.push_back(relative);
            } else if (node->IsComponent() && !isStock(id)) {
                const std::string relative = "components/"
                    + FileStem(node->name, "Component " + std::to_string(++unnamed)) + ".vaecomp";
                if (!WriteRoot(document, id, root / relative, library)) return false;
                project.components.push_back(relative);
            }
        }

        // The theme, the project's own tokens and its assets: document-wide, so they belong to no
        // one screen. Written as a document like everything else rather than folded into the index,
        // because the writer already knows how to write a token and JSON would be a second spelling
        // of the same thing.
        {
            Document shared;
            if (library && !library->Install(library->Id(), library->Version(), shared)) return false;
            shared.SetTheme(document.ActiveTheme());
            for (const auto& [name, token] : document.Tokens()) shared.SetToken(name, token);
            for (const auto& asset : document.Assets()) shared.AddAsset(asset.name, asset.path, asset.id);
            if (!Serializer::Save(shared, root / "tokens.vae", library)) return false;
        }

        RemoveOrphans(root / "screens", project.screens, ".vaescreen");
        RemoveOrphans(root / "components", project.components, ".vaecomp");

        project.root = root;
        return Save(project, projectFile);
    }

    bool Project::LoadDocument(const fs::path& projectFile, Document& out, Project& outProject,
                               std::string* error, const LibrarySource* library) {
        if (!Load(projectFile, outProject, error)) return false;

        out.Clear();
        if (library && !library->Install(library->Id(), library->Version(), out)) {
            if (error) *error = "no component library to build this project against";
            return false;
        }

        const fs::path root = outProject.root;
        // Components first: a screen's instance names its component by id, and the reference is
        // only dangling until the file that defines it has been read.
        const auto readAll = [&](const std::vector<std::string>& files) {
            for (const std::string& relative : files) {
                std::string one;
                if (!Serializer::LoadInto(root / relative, out, &one, library)) {
                    if (error) *error = one;
                    return false;
                }
            }
            return true;
        };

        std::error_code ec;
        if (fs::exists(root / "tokens.vae", ec)
            && !Serializer::LoadInto(root / "tokens.vae", out, error, library)) return false;
        if (!readAll(outProject.components)) return false;
        if (!readAll(outProject.screens)) return false;
        return true;
    }

}
