#include "Panels.h"

#include "vae/ui/Widget.h"

#include <imgui.h>

namespace vae {

    namespace {

        const char* IconFor(const doc::Node& node) {
            // Nerd Font glyphs; the font is loaded with the private-use range so these resolve.
            switch (node.kind) {
                case doc::NodeKind::Screen:    return "";  // display
                case doc::NodeKind::Text:      return "";  // text
                case doc::NodeKind::Image:     return "";  // image
                case doc::NodeKind::Vector:    return "";  // shape
                case doc::NodeKind::Component: return "";  // puzzle piece
                case doc::NodeKind::Instance:  return "";  // cubes
                default:                       return "";  // square
            }
        }

        // `chain` is every instance this row sits inside, outermost first — the same path the
        // flattener resolves overrides through, so what the Inspector edits is what the canvas draws.
        //
        // `slot` is what the nearest enclosing instance handed to the component's slot, with the
        // chain those nodes were authored in. The tree has to show them where they appear, or the
        // contents of every card on the screen are invisible to the panel that lists the screen.
        struct Slot { const std::vector<Uuid>* children = nullptr; std::vector<Uuid> chain; };

        // Whether a row's subtree contains a node, walking the tree the panel actually draws
        // rather than the document's parent links. The two differ wherever a slot is involved:
        // a card's contents are the page's children, but they appear a step further in, inside
        // the component's slot, and the branch that has to open is the one on screen.
        bool Reaches(const doc::Document& d, Uuid id, Uuid target,
                     const std::vector<Uuid>* slot) {
            const doc::Node* node = d.Find(id);
            if (!node) return false;
            if (id == target) return true;

            const doc::Node* component = node->IsInstance() ? d.Find(node->componentId) : nullptr;
            if (node->slot && slot && !slot->empty()) {
                for (Uuid child : *slot) if (Reaches(d, child, target, nullptr)) return true;
                return false;
            }
            if (component) {
                if (!node->children.empty() && d.SlotOf(node->componentId) == component->id) {
                    for (Uuid child : node->children)
                        if (Reaches(d, child, target, nullptr)) return true;
                    return false;
                }
                const std::vector<Uuid>* inner = node->children.empty() ? nullptr
                                                                        : &node->children;
                for (Uuid child : component->children)
                    if (Reaches(d, child, target, inner)) return true;
                return false;
            }
            for (Uuid child : node->children) if (Reaches(d, child, target, slot)) return true;
            return false;
        }

        // The node the panel should be showing, and whether it just changed. Only on the frame it
        // changes, so a branch collapsed by hand stays collapsed.
        Uuid g_Reveal = Uuid::Invalid();
        Uuid g_Revealed = Uuid::Invalid();

        void DrawNode(EditorState& state, Uuid id, std::vector<Uuid>& chain,
                      const Slot* slot = nullptr) {
            const doc::Node* node = state.Doc().Find(id);
            if (!node) return;

            // An instance is not a leaf: it is the component it points at. Expanding one is the only
            // way to reach a card's title, and the only honest picture of a screen built out of
            // components made of components.
            const doc::Node* component = node->IsInstance() ? state.Doc().Find(node->componentId)
                                                            : nullptr;
            const bool fillsSlot = node->slot && slot && slot->children && !slot->children->empty();
            // A component that is its own slot — a grid, a button group — has no innards worth
            // listing: what an instance of it holds *is* what it shows, and the page wrote it.
            // Showing the component's placeholder cells instead hides the only content there is.
            const bool selfSlot = component && !node->children.empty()
                               && state.Doc().SlotOf(node->componentId) == component->id;
            const std::vector<Uuid>& children = fillsSlot ? *slot->children
                                              : selfSlot  ? node->children
                                              : component ? component->children
                                                          : node->children;

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanAvailWidth;
            // An instance stays closed until asked: a screen of cards should read as a list of cards,
            // not as everything every card is made of.
            if (!component || selfSlot) flags |= ImGuiTreeNodeFlags_DefaultOpen;
            if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
            if (state.IsSelected(id) && state.InstancePath() == chain)
                flags |= ImGuiTreeNodeFlags_Selected;

            // Picking something on the canvas has to show up here, or the two panels are telling
            // different stories about the same screen — and everything inside a container is
            // three closed branches deep.
            if (g_Reveal.Valid() && id != g_Reveal
                && Reaches(state.Doc(), id, g_Reveal, slot ? slot->children : nullptr))
                ImGui::SetNextItemOpen(true);

            ImGui::PushID(static_cast<int>(id.Value()));
            if (!node->visible) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);

            const bool open = ImGui::TreeNodeEx("##node", flags, "%s  %s", IconFor(*node),
                                                node->name.c_str());
            if (id == g_Reveal) ImGui::SetScrollHereY(0.5f);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                if (chain.empty()) state.Select(id, ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl);
                else               state.SelectInside(chain, id);
            }

            // Which component a row came from, dimmed and after the name — the answer to "where does
            // this actually live" without opening anything.
            if (component) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", component->name.c_str());
            } else if (node->slot) {
                ImGui::SameLine();
                ImGui::TextDisabled("slot");
            }

            // Marking a slot is a fact about the component, so it belongs on the node rather than
            // in a dialog somewhere else; and an instance is the obvious place to ask to open the
            // thing it is an instance of.
            const Uuid owner = state.ComponentOwning(id);
            if ((component || (owner.Valid() && owner != id))
                && ImGui::BeginPopupContextItem("##row")) {
                if (component && ImGui::MenuItem("Edit this component"))
                    state.OpenComponent(node->componentId);
                if (owner.Valid() && owner != id) {
                    if (ImGui::MenuItem("Use as the slot", nullptr, node->slot))
                        state.SetSlot(id, !node->slot);
                    ImGui::TextDisabled("Where an instance's own children go.");
                }
                ImGui::EndPopup();
            }

            // The eye sits at the right edge, so the row still reads as a name and not as a toolbar.
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 22.0f);
            if (ImGui::SmallButton(node->visible ? "" : "")) {
                doc::Node* mutableNode = state.Doc().Find(id);
                mutableNode->visible = !mutableNode->visible;
                state.Doc().Touch(id);
            }

            if (!node->visible) ImGui::PopStyleVar();

            if (open) {
                if (fillsSlot) {
                    // Slot content belongs to the page, not to the component it is showing inside,
                    // so it is drawn in the scope it was authored in.
                    std::vector<Uuid> outer = slot->chain;
                    for (Uuid child : children) DrawNode(state, child, outer);
                } else if (selfSlot) {
                    // Same scope as the instance itself: these are the page's nodes, not a step
                    // deeper into a component.
                    for (Uuid child : children) DrawNode(state, child, chain);
                } else if (component) {
                    Slot content;
                    if (!node->children.empty()) {
                        content.children = &node->children;
                        content.chain = chain;
                    }
                    chain.push_back(id);
                    for (Uuid child : children)
                        DrawNode(state, child, chain, content.children ? &content : nullptr);
                    chain.pop_back();
                } else {
                    for (Uuid child : children) DrawNode(state, child, chain, slot);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

    }

    void DrawLayersPanel(EditorState& state) {
        ImGui::Begin("Layers###Layers");

        const Uuid screen = state.ActiveScreen();
        if (!state.Doc().Contains(screen)) {
            ImGui::TextDisabled("No screen.");
            ImGui::End();
            return;
        }

        const Uuid primary = state.Primary();
        g_Reveal = primary != g_Revealed ? primary : Uuid::Invalid();
        g_Revealed = primary;

        std::vector<Uuid> chain;
        DrawNode(state, screen, chain);

        // Clicking the empty space below the tree clears the selection, which is the gesture people
        // reach for without being told.
        ImGui::InvisibleButton("##empty", ImVec2(-1.0f, std::max(ImGui::GetContentRegionAvail().y, 1.0f)));
        if (ImGui::IsItemClicked()) state.ClearSelection();

        ImGui::End();
    }

}
