#include "vaepch.h"
#include "vae/ui/Library.h"

#include <variant>

namespace vae::ui {

    namespace {

        constexpr std::string_view kLibraryId = "vae.std";
        // Bumped when the catalog changes in a way that moves the ids BuildStandardLibrary mints,
        // which is what an override inside an instance is keyed by. Recorded in every document so a
        // future migration has something to key off; a document naming an older version still
        // loads, because the components it forked are in the file and the rest are rebuilt.
        constexpr u32 kLibraryVersion = 1;

        // One built copy of the catalog, kept to answer "is this still the stock widget?". Building
        // it is ~480 nodes and happens once per process rather than once per save.
        struct Reference {
            doc::Document document;
            Library index;
            Reference() { index = BuildStandardLibrary(document); }
        };

        const Reference& Pristine() {
            static const Reference reference;
            return reference;
        }

        class StandardLibrarySource final : public doc::LibrarySource {
        public:
            std::string_view Id() const override { return kLibraryId; }
            u32 Version() const override { return kLibraryVersion; }

            bool Install(std::string_view id, u32 version, doc::Document& into) const override {
                if (id != kLibraryId || version > kLibraryVersion) return false;
                BuildStandardLibrary(into);
                return true;
            }

            // The reference document has them because BuildStandardLibrary installs them, so
            // there is one list rather than two that could drift apart.
            const std::map<std::string, doc::Token>& Tokens() const override {
                return Pristine().document.Tokens();
            }

            std::vector<Uuid> Stock(const doc::Document& document) const override {
                const Reference& reference = Pristine();
                std::vector<Uuid> stock;
                for (Uuid root : document.Roots()) {
                    const doc::Node* node = document.Find(root);
                    if (!node || !node->IsComponent()) continue;
                    if (!reference.document.Contains(root)) continue;

                    // Ids are deterministic, so an untouched component matches whole — including
                    // the ids — and a comparison that walks both subtrees in lockstep is enough.
                    const std::vector<Uuid> mine = document.Subtree(root);
                    const std::vector<Uuid> theirs = reference.document.Subtree(root);
                    if (mine != theirs) continue;

                    const bool identical = std::all_of(mine.begin(), mine.end(), [&](Uuid id) {
                        const doc::Node* a = document.Find(id);
                        const doc::Node* b = reference.document.Find(id);
                        return a && b && *a == *b;
                    });
                    if (identical) stock.push_back(root);
                }
                return stock;
            }
        };

    }

    const doc::LibrarySource& StandardLibrary() {
        static const StandardLibrarySource source;
        return source;
    }

}
