#include "vaepch.h"
#include "vae/doc/Serializer.h"

#include "vae/base/FileSystem.h"
#include "vae/doc/ValueText.h"

#include <pugixml.hpp>

#include <algorithm>

// What is left here is everything that is not the codec: reading and writing the file, the project
// index, and putting a project split across files back together. The codec is XmlCodec.cpp, and it
// is the only one.
namespace vae::doc {

    namespace fs = std::filesystem;

    bool Serializer::Save(const Document& document, const fs::path& path,
                          const LibrarySource* library) {
        return FileSystem::WriteText(path, ToXml(document, true, library));
    }

    bool Serializer::Load(const fs::path& path, Document& out, std::string* error,
                          const LibrarySource* library) {
        // A directory is the commonest wrong argument — someone points at the project folder rather
        // than into it — and it deserves an answer about folders rather than a parse failure.
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            if (error) {
                const fs::path index = Project::FileIn(path);
                *error = index.empty()
                       ? path.filename().string() + " is a folder and holds no "
                         + std::string(Project::kFileName)
                       : path.filename().string() + " is a folder — open "
                         + index.filename().string() + " inside it";
            }
            return false;
        }

        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        return FromXml(*text, out, error, library);
    }

    bool Serializer::LoadInto(const fs::path& path, Document& out, std::string* error,
                              const LibrarySource* library) {
        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        return FromXml(*text, out, error, library, true);
    }

    // ---------------------------------------------------------------- Project

    bool Project::Save(const Project& project, const fs::path& path) {
        std::string out = "<vae version=\"" + std::to_string(Serializer::kFormatVersion) + "\">\n";
        out += "  <project name=\"" + text::EscapeAttr(project.name) + "\"";
        out += " script=\"" + text::EscapeAttr(project.scriptLanguage) + "\"";
        out += " targetSize=\"" + text::Vec2Text(project.targetResolution) + "\"";
        if (project.fontDirs.empty()) {
            out += "/>\n";
        } else {
            out += ">\n";
            for (const std::string& dir : project.fontDirs)
                out += "    <fonts dir=\"" + text::EscapeAttr(dir) + "\"/>\n";
            out += "  </project>\n";
        }
        out += "</vae>\n";
        return FileSystem::WriteText(path, out);
    }

    bool Project::Load(const fs::path& path, Project& out, std::string* error) {
        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }

        pugi::xml_document xml;
        const pugi::xml_parse_result parsed = xml.load_buffer(text->data(), text->size());
        if (!parsed) {
            if (error) *error = std::string("project file: ") + parsed.description();
            return false;
        }

        const pugi::xml_node root = xml.child("vae");
        if (!root) {
            if (error) *error = "not a VAE project (no <vae> root)";
            return false;
        }
        const auto version = root.attribute("version").as_uint(0);
        if (version != Serializer::kFormatVersion) {
            if (error)
                *error = "project is format " + std::to_string(version) + "; this VAE reads "
                       + std::to_string(Serializer::kFormatVersion);
            return false;
        }

        const pugi::xml_node project = root.child("project");
        if (!project) {
            if (error) *error = "not a VAE project (no <project> element)";
            return false;
        }

        out.name = project.attribute("name").as_string("Untitled");
        out.scriptLanguage = project.attribute("script").as_string("lua");
        out.root = path.parent_path();
        out.fontDirs.clear();
        for (pugi::xml_node fonts : project.children("fonts"))
            out.fontDirs.emplace_back(fonts.attribute("dir").as_string());
        if (const auto size = text::Vec2FromText(project.attribute("targetSize").as_string()))
            out.targetResolution = *size;
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
                else if (c == ' ' && !stem.empty() && stem.back() != ' ') stem.push_back(c);
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

        // Documents in `dir` this save did not write. Left behind, a deleted screen still loads on
        // the next open — the folder is what the loader reads, so the folder is what a save has to
        // leave correct.
        void RemoveOrphans(const fs::path& dir, const std::vector<fs::path>& written) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.path().extension() != Serializer::kExtension) continue;
                if (std::find(written.begin(), written.end(), entry.path()) == written.end())
                    fs::remove(entry.path(), ec);
            }
        }

        // Every .vae file directly inside `dir`, sorted. Sorted so two machines agree about the
        // order a project loads in, which is what keeps ids and diffs stable.
        std::vector<fs::path> DocumentsDirectlyIn(const fs::path& dir) {
            std::vector<fs::path> out;
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return out;
            for (const auto& entry : fs::directory_iterator(dir, ec))
                if (entry.is_regular_file(ec) && entry.path().extension() == Serializer::kExtension)
                    out.push_back(entry.path());
            std::sort(out.begin(), out.end());
            return out;
        }

    }

    bool Project::IsProjectFile(const fs::path& path) { return path.filename() == kFileName; }

    fs::path Project::FileIn(const fs::path& dir) {
        std::error_code ec;
        const fs::path candidate = dir / kFileName;
        return fs::is_regular_file(candidate, ec) ? candidate : fs::path{};
    }

    std::vector<fs::path> Project::DocumentsIn(const fs::path& root) {
        // Components first: a screen's instance names its component by id, and the reference is
        // only dangling until the file that defines it has been read.
        std::vector<fs::path> out = DocumentsDirectlyIn(root / "components");
        const std::vector<fs::path> screens = DocumentsDirectlyIn(root / "screens");
        out.insert(out.end(), screens.begin(), screens.end());
        return out;
    }

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

        std::vector<fs::path> screens;
        std::vector<fs::path> components;
        u32 unnamed = 0;

        for (Uuid id : document.Roots()) {
            const Node* node = document.Find(id);
            if (!node) continue;

            if (node->kind == NodeKind::Screen) {
                const fs::path path = root / "screens"
                    / (FileStem(node->name, "Screen " + std::to_string(++unnamed))
                       + std::string(Serializer::kExtension));
                if (!WriteRoot(document, id, path, library)) return false;
                screens.push_back(path);
            } else if (node->IsComponent() && !isStock(id)) {
                const fs::path path = root / "components"
                    / (FileStem(node->name, "Component " + std::to_string(++unnamed))
                       + std::string(Serializer::kExtension));
                if (!WriteRoot(document, id, path, library)) return false;
                components.push_back(path);
            }
        }

        // The theme, the project's own tokens and its assets: document-wide, so they belong to no
        // one screen. Written as a document like everything else.
        {
            Document shared;
            if (library && !library->Install(library->Id(), library->Version(), shared)) return false;
            shared.SetTheme(document.ActiveTheme());
            for (const auto& [name, token] : document.Tokens()) shared.SetToken(name, token);
            for (const auto& asset : document.Assets()) shared.AddAsset(asset.name, asset.path, asset.id);
            if (!Serializer::Save(shared, root / "tokens.vae", library)) return false;
        }

        RemoveOrphans(root / "screens", screens);
        RemoveOrphans(root / "components", components);

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
        std::error_code ec;
        if (fs::exists(root / "tokens.vae", ec)
            && !Serializer::LoadInto(root / "tokens.vae", out, error, library)) return false;

        for (const fs::path& file : DocumentsIn(root)) {
            std::string one;
            if (!Serializer::LoadInto(file, out, &one, library)) {
                if (error) *error = file.filename().string() + ": " + one;
                return false;
            }
        }
        return true;
    }

}
