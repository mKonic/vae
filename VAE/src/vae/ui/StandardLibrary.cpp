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

        // Same tree, node for node, ignoring the ids — which is the question a document written
        // before format 2 asks, because its copy of the catalog was minted with random ids and
        // nothing about them lines up with what the binary now builds. `pairs` comes back as the
        // old-to-new mapping, in traversal order, so the references into the copy can be rewritten.
        bool SameShape(const doc::Document& a, Uuid ia, const doc::Document& b, Uuid ib,
                       std::vector<std::pair<Uuid, Uuid>>& pairs) {
            const doc::Node* x = a.Find(ia);
            const doc::Node* y = b.Find(ib);
            if (!x || !y) return false;
            if (x->kind != y->kind || x->name != y->name) return false;
            if (!(x->layout == y->layout) || !(x->props == y->props)) return false;
            if (x->visible != y->visible || x->locked != y->locked || x->slot != y->slot) return false;
            if (x->componentId.Valid() != y->componentId.Valid()) return false;
            // No component in the catalog instances another, so an override table here means the
            // subtree is not the stock one whatever else matches.
            if (!x->overrides.empty() || !y->overrides.empty()) return false;
            if (x->children.size() != y->children.size()) return false;

            pairs.emplace_back(ia, ib);
            for (std::size_t i = 0; i < x->children.size(); ++i)
                if (!SameShape(a, x->children[i], b, y->children[i], pairs)) return false;
            return true;
        }

        std::vector<Uuid> AllNodes(const doc::Document& document) {
            std::vector<Uuid> out;
            for (Uuid root : document.Roots()) {
                const std::vector<Uuid> subtree = document.Subtree(root);
                out.insert(out.end(), subtree.begin(), subtree.end());
            }
            return out;
        }

        // Every place a document can name a node that lives inside a component: what an instance
        // points at, what its overrides are filed under, and any property holding a node reference.
        void RemapReferences(doc::Document& document, const std::unordered_map<Uuid, Uuid>& remap) {
            auto Mapped = [&](Uuid id) {
                auto it = remap.find(id);
                return it == remap.end() ? id : it->second;
            };
            for (Uuid id : AllNodes(document)) {
                doc::Node* node = document.Find(id);
                if (!node) continue;

                if (node->componentId.Valid()) node->componentId = Mapped(node->componentId);

                if (!node->overrides.empty()) {
                    std::map<Uuid, doc::PropBag> rekeyed;
                    for (auto& [target, props] : node->overrides)
                        rekeyed[Mapped(target)] = std::move(props);
                    node->overrides = std::move(rekeyed);
                }

                std::vector<std::pair<doc::Prop, Uuid>> known;
                for (const auto& [prop, value] : node->props.Known())
                    if (const Uuid* ref = std::get_if<Uuid>(&value); ref && remap.contains(*ref))
                        known.emplace_back(prop, Mapped(*ref));
                for (const auto& [prop, ref] : known) node->props.Set(prop, ref);

                std::vector<std::pair<std::string, Uuid>> custom;
                for (const auto& [key, value] : node->props.Custom())
                    if (const Uuid* ref = std::get_if<Uuid>(&value); ref && remap.contains(*ref))
                        custom.emplace_back(key, Mapped(*ref));
                for (const auto& [key, ref] : custom) node->props.Set(key, ref);
            }
        }

        // Copies a component subtree across, ids intact. Children are wired in a second pass
        // because InsertNode appends to the parent it is given, so a node that arrived carrying its
        // own child list would end up holding every child twice.
        void CopySubtree(const doc::Document& from, Uuid root, doc::Document& into) {
            const std::vector<Uuid> subtree = from.Subtree(root);
            for (Uuid id : subtree) {
                doc::Node copy = *from.Find(id);
                copy.children.clear();
                into.InsertNode(std::move(copy));
            }
            for (Uuid id : subtree)
                if (doc::Node* node = into.Find(id)) node->children = from.Find(id)->children;
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

            u32 Adopt(doc::Document& document) const override {
                const Reference& reference = Pristine();

                struct Fold {
                    Uuid oldRoot;
                    Uuid newRoot;
                    std::vector<std::pair<Uuid, Uuid>> pairs;
                };
                std::vector<Fold> folds;

                for (Uuid root : document.Roots()) {
                    const doc::Node* node = document.Find(root);
                    if (!node || !node->IsComponent()) continue;

                    auto it = reference.index.components.find(node->name);
                    if (it == reference.index.components.end()) continue;
                    // Already the stock component, or the stock one is here beside it: nothing to
                    // fold, and overwriting would be the wrong move either way.
                    if (root == it->second || document.Contains(it->second)) continue;

                    std::vector<std::pair<Uuid, Uuid>> pairs;
                    if (!SameShape(document, root, reference.document, it->second, pairs)) continue;
                    folds.push_back({ root, it->second, std::move(pairs) });
                }
                if (folds.empty()) return 0;

                std::unordered_map<Uuid, Uuid> remap;
                for (const Fold& fold : folds)
                    for (const auto& [from, to] : fold.pairs) remap.emplace(from, to);

                // Rewrite first, while the nodes being pointed at still exist.
                RemapReferences(document, remap);
                for (const Fold& fold : folds) {
                    document.DeleteNode(fold.oldRoot);
                    CopySubtree(reference.document, fold.newRoot, document);
                }
                return static_cast<u32>(folds.size());
            }
        };

    }

    const doc::LibrarySource& StandardLibrary() {
        static const StandardLibrarySource source;
        return source;
    }

}
