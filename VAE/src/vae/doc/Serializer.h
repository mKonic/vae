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

        // Folds a document that carries an inlined copy of the catalog (everything written before
        // format 2) down to a reference, rewriting the instance and override ids that pointed into
        // the copy. Returns how many components were folded. Without this the saving would only
        // ever apply to files made from here on.
        virtual u32 Adopt(Document& document) const = 0;
    };

    // Markup, not JSON. The same 61-node app is 32 KB / 1,569 lines as JSON and 10.6 KB / 124 lines
    // as XML, and the line count is the one that matters: a file you can read as an outline of the
    // app beats one you can only search. The element name is the node kind, the tree is the
    // indentation, and enums are names rather than the raw u8 JSON was writing.
    //
    // What the encoder does NOT write is most of the format: a layout field equal to its default,
    // a value's type where the property already declares it, an id nothing references, and any
    // component the library above can rebuild. design/xml-format.md has the measurements.
    class Serializer {
    public:
        // Bumped when the on-disk shape changes in a way an older reader could misread. Documents
        // written by a NEWER version are refused rather than half-read.
        static constexpr u32 kFormatVersion = 3;
        // What the JSON encoder stamps, and the highest it will read. Format 3 is markup, so a JSON
        // file is by definition format 2 or older; the two numbers are separate because the JSON
        // codec is now a reader for old files rather than the current format.
        static constexpr u32 kJsonFormatVersion = 2;

        // XML, format 3. This is what a project saves as.
        //
        // `keepIds` writes an id on every node instead of only on the ones something references.
        // A file does not want them — 6 of Vaecord's 540 ids are ever referred to, and the rest are
        // noise in a diff — but an in-memory round trip does: the Play/Stop snapshot restores the
        // document in place, and every observer that survives that is holding an id.
        static std::string ToXml(const Document& document, bool pretty = true,
                                 const LibrarySource* library = nullptr, bool keepIds = false);
        static bool FromXml(std::string_view xml, Document& out, std::string* error = nullptr,
                            const LibrarySource* library = nullptr);

        // JSON, format 2 and older. Kept as the migration path for every file that already exists,
        // and as the shape a Figma import arrives in.
        static std::string ToJson(const Document& document, bool pretty = true,
                                  const LibrarySource* library = nullptr);
        static bool FromJson(std::string_view json, Document& out, std::string* error = nullptr,
                             const LibrarySource* library = nullptr);

        // Reads either, by looking at the first non-space character: '<' is markup, '{' is JSON.
        static bool FromText(std::string_view text, Document& out, std::string* error = nullptr,
                             const LibrarySource* library = nullptr);

        static bool Save(const Document& document, const std::filesystem::path& path,
                         const LibrarySource* library = nullptr);
        static bool Load(const std::filesystem::path& path, Document& out,
                         std::string* error = nullptr, const LibrarySource* library = nullptr);
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
