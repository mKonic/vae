#pragma once

#include "vae/doc/Document.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vae::doc {

    // Components a document names instead of carrying. The standard widget catalog is 53
    // components and ~480 nodes; copied into every screen file it was 89% of the bytes, and worse,
    // it pinned every saved project to the catalog as it stood the day it was saved — a later fix
    // to Button could never reach a file that already existed. A document that references a
    // library gets it rebuilt from the binary on load instead.
    //
    // Implemented by vae::ui, which owns the catalog; the doc layer only knows the shape, so the
    // dependency stays pointing the right way.
    struct LibrarySource {
        virtual ~LibrarySource() = default;

        virtual std::string_view Id() const = 0;
        virtual u32 Version() const = 0;

        // Builds the named library into `into`. False when this source does not have that library,
        // or does not have it at a version it can honour.
        virtual bool Install(std::string_view id, u32 version, Document& into) const = 0;

        // The tokens Install puts in. A document that did not touch them need not write them —
        // and one that deleted a default has to write the deletion, or Install would hand it
        // straight back on the next load.
        virtual const std::map<std::string, Token>& Tokens() const = 0;

        // Which of `document`'s components are still exactly what Install would build, and so need
        // not be written to the file. A component the designer edited is not one of these: it is a
        // fork, and a fork is written out in full.
        virtual std::vector<Uuid> Stock(const Document& document) const = 0;
    };

    // Markup. One codec, one extension, one thing a file can be — see D14.
    //
    // The element name is the node kind, the tree is the indentation, and enums are names. What the
    // encoder does NOT write is most of the format: a layout field equal to its default, a value's
    // type where the property already declares it, an id nothing references, and any component the
    // library above can rebuild. `design/xml-format.md` has the measurements.
    class Serializer {
    public:
        // Every file VAE authors carries this. Documents written by a NEWER version are refused
        // rather than half-read; older ones are refused too, because there is nothing older left to
        // read — format 4 is the only format.
        static constexpr u32 kFormatVersion = 4;

        // The extension, and the only one. What a file holds is what is inside it and which folder
        // it sits in — a screen and a component are the same kind of file.
        static constexpr std::string_view kExtension = ".vae";

        // `keepIds` writes an id on every node instead of only on the ones something references.
        // A file does not want them — 6 of Vaecord's 540 ids are ever referred to, and the rest are
        // noise in a diff — but an in-memory round trip does: the Play/Stop snapshot restores the
        // document in place, and every observer that survives that is holding an id.
        static std::string ToXml(const Document& document, bool pretty = true,
                                 const LibrarySource* library = nullptr, bool keepIds = false);
        // `merge` reads into whatever `out` already holds instead of replacing it, and skips
        // installing the library because it is already there. Use LoadInto rather than this.
        static bool FromXml(std::string_view xml, Document& out, std::string* error = nullptr,
                            const LibrarySource* library = nullptr, bool merge = false);

        // One node and everything under it, as the markup a document holds for it — with no <vae>
        // wrapper, because the wrapper belongs to the file and this is a view of a selection. Ids
        // are always written: what comes back is spliced into a live document, and every observer,
        // the selection and every override key is holding one.
        static std::string ToXmlSubtree(const Document& document, Uuid node);

        // Reads markup back over `node`: same id, same parent, same place among its siblings, with
        // everything under it replaced by what the markup says. The root element keeps the id it is
        // replacing whatever the markup claims — you are editing this node, not describing a
        // different one, and a changed id is a node the rest of the document has lost track of.
        //
        // Nothing is touched unless the markup parses, so a typo costs a message rather than the
        // subtree.
        static bool FromXmlSubtree(std::string_view xml, Document& out, Uuid node,
                                   std::string* error = nullptr);

        static bool Save(const Document& document, const std::filesystem::path& path,
                         const LibrarySource* library = nullptr);
        // Refuses a directory by saying so, rather than by failing to parse one.
        static bool Load(const std::filesystem::path& path, Document& out,
                         std::string* error = nullptr, const LibrarySource* library = nullptr);

        // Reads a file into a document that already holds something, rather than replacing it.
        // This is how a project split across files is put back together: its parts are ordinary
        // documents, and a screen whose `of=` names a component from another file simply resolves
        // once that file has been read.
        //
        // The library must already be installed in `out`. Installing it a second time would mint
        // the ids it already minted, so this never does it.
        static bool LoadInto(const std::filesystem::path& path, Document& out,
                             std::string* error = nullptr,
                             const LibrarySource* library = nullptr);
    };

    // A project's settings, and nothing else.
    //
    // It deliberately does NOT list the project's files. `screens/` and `components/` are what
    // exists; an index that also claims to know is a second answer that can disagree with the first,
    // and the disagreement is always the index being wrong.
    struct Project {
        std::string name = "Untitled";
        std::string scriptLanguage = "lua";     // "lua", "cpp" or "blueprint", chosen once
        std::filesystem::path root;
        std::vector<std::string> fontDirs;
        Vec2 targetResolution{ 1280.0f, 800.0f };

        // The one file that makes a folder a project. Written as markup like everything else — the
        // writer already knows how to write a document, and a second spelling would be a second
        // parser to keep in step.
        static constexpr std::string_view kFileName = "project.vae";

        static bool Save(const Project& project, const std::filesystem::path& path);
        static bool Load(const std::filesystem::path& path, Project& out, std::string* error = nullptr);

        // --- a project split across files ---------------------------------------------------
        //
        // One document in memory, many files on disk: `screens/<Name>.vae` each holding one screen,
        // `components/<Name>.vae` each holding one forked component, and `tokens.vae` holding the
        // theme, the project's tokens and its assets.
        //
        // The point is the diff. A thirty-screen app in one file means every edit touches that
        // file, so a version-control history says nothing about which screen changed and two
        // people cannot edit different screens without conflicting.
        //
        // Nothing new is serialized to make this work: every part is an ordinary document that the
        // ordinary loader reads, which is why a screen file can also just be opened on its own.
        static bool SaveDocument(const Document& document, Project& project,
                                 const std::filesystem::path& projectFile,
                                 const LibrarySource* library = nullptr);
        static bool LoadDocument(const std::filesystem::path& projectFile, Document& out,
                                 Project& outProject, std::string* error = nullptr,
                                 const LibrarySource* library = nullptr);

        // The documents under `root`, in the order they should be read: components before screens,
        // because a screen's instance names a component that must already be there. Alphabetical
        // within each, so two machines agree and a diff is stable.
        static std::vector<std::filesystem::path> DocumentsIn(const std::filesystem::path& root);

        // True when the path names the project index rather than a single document.
        static bool IsProjectFile(const std::filesystem::path& path);
        // The project file inside `dir`, or empty when `dir` is not a project folder. This is what
        // "open this folder" means.
        static std::filesystem::path FileIn(const std::filesystem::path& dir);
    };

}
