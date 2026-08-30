#include "vaepch.h"
#include "vae/doc/Command.h"
#include "vae/doc/Serializer.h"

#include <algorithm>

namespace vae::doc {

    // ---------------------------------------------------------------- SetProp

    void SetPropCommand::Apply(Document& document) {
        if (!m_Captured) { m_Old = document.GetProp(m_Node, m_Prop); m_Captured = true; }
        document.SetProp(m_Node, m_Prop, m_New);
    }

    void SetPropCommand::Undo(Document& document) { document.SetProp(m_Node, m_Prop, m_Old); }

    bool SetPropCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetPropCommand*>(&newer);
        if (!other || other->m_Node != m_Node || other->m_Prop != m_Prop) return false;
        // Keep the ORIGINAL old value: undoing a coalesced drag must return to where it started,
        // not to the previous mouse position.
        m_New = other->m_New;
        return true;
    }

    // ---------------------------------------------------------------- SetBlueprint

    void SetBlueprintCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const Blueprint* existing = document.BlueprintFor(m_Node)) m_Old = *existing;
            m_Captured = true;
        }
        document.SetBlueprint(m_Node, m_New);
    }

    void SetBlueprintCommand::Undo(Document& document) { document.SetBlueprint(m_Node, m_Old); }

    bool SetBlueprintCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetBlueprintCommand*>(&newer);
        // Only a run of the same edit on the same blueprint: dragging a node is a hundred of these and
        // wants to be one undo, but a drag followed by a delete is two things that happened.
        if (!other || other->m_Node != m_Node || other->m_Name != m_Name) return false;
        m_New = other->m_New;
        return true;
    }

    // ---------------------------------------------------------------- SetOverride

    void SetOverrideCommand::Apply(Document& document) {
        if (!m_Captured) {
            const Node* node = document.Find(m_Instance);
            if (node) {
                auto it = node->overrides.find(m_Node);
                if (it != node->overrides.end())
                    if (const Value* existing = it->second.Find(m_Prop)) {
                        m_Old = *existing;
                        m_HadOld = true;
                    }
            }
            m_Captured = true;
        }
        document.SetOverride(m_Instance, m_Node, m_Prop, m_New);
    }

    void SetOverrideCommand::Undo(Document& document) {
        // "No override" is not the same as "override set to the old value" — clearing it is what
        // puts the instance back under the component's control.
        if (m_HadOld) document.SetOverride(m_Instance, m_Node, m_Prop, m_Old);
        else          document.ClearOverride(m_Instance, m_Node, m_Prop);
    }

    bool SetOverrideCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetOverrideCommand*>(&newer);
        if (!other || other->m_Instance != m_Instance || other->m_Node != m_Node
            || other->m_Prop != m_Prop) return false;
        m_New = other->m_New;
        return true;
    }

    void SetKeyedPropCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const Node* node = document.Find(m_Node))
                if (const Value* existing = node->props.Find(m_Key)) m_Old = *existing;
            m_Captured = true;
        }
        document.SetProp(m_Node, m_Key, m_New);
    }

    void SetKeyedPropCommand::Undo(Document& document) {
        document.SetProp(m_Node, m_Key, m_Old);
    }

    bool SetKeyedPropCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetKeyedPropCommand*>(&newer);
        if (!other || other->m_Node != m_Node || other->m_Key != m_Key) return false;
        m_New = other->m_New;
        return true;
    }

    void SetKeyedOverrideCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const Node* node = document.Find(m_Instance)) {
                auto it = node->overrides.find(m_Node);
                if (it != node->overrides.end())
                    if (const Value* existing = it->second.Find(m_Key)) {
                        m_Old = *existing;
                        m_HadOld = true;
                    }
            }
            m_Captured = true;
        }
        document.SetOverride(m_Instance, m_Node, m_Key, m_New);
    }

    void SetKeyedOverrideCommand::Undo(Document& document) {
        if (m_HadOld) document.SetOverride(m_Instance, m_Node, m_Key, m_Old);
        else          document.ClearOverride(m_Instance, m_Node, m_Key);
    }

    bool SetKeyedOverrideCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetKeyedOverrideCommand*>(&newer);
        if (!other || other->m_Instance != m_Instance || other->m_Node != m_Node
            || other->m_Key != m_Key) return false;
        m_New = other->m_New;
        return true;
    }

    // ---------------------------------------------------------------- SetLayout

    void SetLayoutCommand::Apply(Document& document) {
        Node* node = document.Find(m_Node);
        if (!node) return;
        if (!m_Captured) { m_Old = node->layout; m_Captured = true; }
        node->layout = m_New;
        document.Touch(m_Node);
    }

    void SetLayoutCommand::Undo(Document& document) {
        if (Node* node = document.Find(m_Node)) {
            node->layout = m_Old;
            document.Touch(m_Node);
        }
    }

    bool SetLayoutCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetLayoutCommand*>(&newer);
        if (!other || other->m_Node != m_Node) return false;
        m_New = other->m_New;
        return true;
    }

    // ---------------------------------------------------------------- Create / Delete

    void CreateNodeCommand::Apply(Document& document) {
        if (m_Created.Valid()) {
            // Redo: recreate with the SAME id, so anything that referenced it still resolves.
            Node node;
            node.id = m_Created;
            node.kind = m_Kind;
            node.name = m_Name;
            node.parent = m_Parent;
            document.InsertNode(std::move(node));
            return;
        }
        m_Created = document.CreateNode(m_Kind, m_Parent, m_Name);
    }

    void CreateNodeCommand::Undo(Document& document) { document.DeleteNode(m_Created); }

    void CreateInstanceCommand::Apply(Document& document) {
        if (m_Snapshot) {
            document.InsertNode(*m_Snapshot);
            return;
        }
        m_Created = document.CreateInstance(m_Component, m_Parent);
        if (m_Created.Valid() && !m_Name.empty()) {
            document.Find(m_Created)->name = m_Name;
            document.Touch(m_Created);
        }
    }

    void CreateInstanceCommand::Undo(Document& document) {
        if (const Node* node = document.Find(m_Created)) m_Snapshot = *node;
        document.DeleteNode(m_Created);
    }

    void DeleteNodeCommand::Apply(Document& document) {
        m_Removed.clear();
        m_Index = document.IndexInParent(m_Node);
        for (Uuid id : document.Subtree(m_Node))
            if (const Node* node = document.Find(id)) m_Removed.push_back(*node);

        document.DeleteNode(m_Node);
    }

    void DeleteNodeCommand::Undo(Document& document) {
        if (m_Removed.empty()) return;

        // Parents first (Subtree guarantees that order), each with its children list intact. The
        // list is restored verbatim rather than rebuilt, so sibling order survives.
        for (std::size_t i = 0; i < m_Removed.size(); ++i) {
            Node node = m_Removed[i];
            const auto children = node.children;
            node.children.clear();
            document.InsertNode(std::move(node), i == 0 ? m_Index : UINT32_MAX);
            if (Node* restored = document.Find(m_Removed[i].id)) restored->children = children;
        }
        // InsertNode appended each child to its parent as well; rebuild the exact lists.
        for (const Node& original : m_Removed)
            if (Node* restored = document.Find(original.id)) restored->children = original.children;
    }

    // ---------------------------------------------------------------- Reparent / Rename

    void ReparentCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const Node* node = document.Find(m_Node)) m_OldParent = node->parent;
            m_OldIndex = document.IndexInParent(m_Node);
            m_Captured = true;
        }
        document.Reparent(m_Node, m_NewParent, m_NewIndex);
    }

    void ReparentCommand::Undo(Document& document) {
        document.Reparent(m_Node, m_OldParent, m_OldIndex);
    }

    void RenameCommand::Apply(Document& document) {
        Node* node = document.Find(m_Node);
        if (!node) return;
        if (!m_Captured) { m_Old = node->name; m_Captured = true; }
        node->name = m_New;
        document.Touch(m_Node);
    }

    void RenameCommand::Undo(Document& document) {
        if (Node* node = document.Find(m_Node)) {
            node->name = m_Old;
            document.Touch(m_Node);
        }
    }

    namespace {
        // Every place a token name can appear in a property: on a node, and inside an instance's
        // overrides. Used by the rename, which has to find all of them or leave half a document
        // pointing at a name that no longer exists.
        void RewriteTokenRefs(Document& document, std::string_view from, std::string_view to) {
            for (Uuid id : document.AllNodes()) {
                Node* node = document.Find(id);
                if (!node) continue;
                bool touched = false;

                const auto rewrite = [&](PropBag& bag) {
                    for (const auto& [prop, value] : bag.Known())
                        if (const TokenRef* ref = std::get_if<TokenRef>(&value); ref && ref->name == from) {
                            bag.Set(prop, TokenRef{ std::string(to) });
                            touched = true;
                        }
                    for (const auto& [key, value] : bag.Custom())
                        if (const TokenRef* ref = std::get_if<TokenRef>(&value); ref && ref->name == from) {
                            bag.Set(key, TokenRef{ std::string(to) });
                            touched = true;
                        }
                };
                rewrite(node->props);
                for (auto& [target, bag] : node->overrides) { (void)target; rewrite(bag); }

                if (touched) document.Touch(id);
            }
        }
    }

    void SetTokenCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const Token* existing = document.FindToken(m_Name)) { m_Old = *existing; m_Existed = true; }
            m_Captured = true;
        }
        document.SetToken(m_Name, m_New);
    }

    void SetTokenCommand::Undo(Document& document) {
        if (m_Existed) document.SetToken(m_Name, m_Old);
        else           document.RemoveToken(m_Name);
    }

    void ReplaceSubtreeCommand::Apply(Document& document) {
        if (!m_Captured) { m_Old = Serializer::ToXmlSubtree(document, m_Node); m_Captured = true; }
        Serializer::FromXmlSubtree(m_New, document, m_Node);
    }

    void ReplaceSubtreeCommand::Undo(Document& document) {
        Serializer::FromXmlSubtree(m_Old, document, m_Node);
    }

    void SetBreakpointsCommand::Apply(Document& document) {
        if (!m_Captured) { m_Old = document.Breakpoints(); m_Captured = true; }
        document.SetBreakpoints(m_New);
    }

    void SetBreakpointsCommand::Undo(Document& document) { document.SetBreakpoints(m_Old); }

    bool SetBreakpointsCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetBreakpointsCommand*>(&newer);
        if (!other) return false;
        m_New = other->m_New;
        return true;
    }

    // Dragging through a colour picker is one edit, the same way dragging a node is one move.
    bool SetTokenCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetTokenCommand*>(&newer);
        if (!other || other->m_Name != m_Name) return false;
        m_New = other->m_New;
        return true;
    }

    void SetComponentPropertyCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const ComponentProperty* existing = document.FindProperty(m_Component, m_New.name)) {
                m_Old = *existing;
                m_Existed = true;
            }
            m_Captured = true;
        }
        document.SetComponentProperty(m_Component, m_New);
    }

    void SetComponentPropertyCommand::Undo(Document& document) {
        if (m_Existed) document.SetComponentProperty(m_Component, m_Old);
        else           document.RemoveComponentProperty(m_Component, m_New.name);
    }

    // Typing a default, or dragging through a picker for one, is one edit.
    bool SetComponentPropertyCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetComponentPropertyCommand*>(&newer);
        if (!other || other->m_Component != m_Component || other->m_New.name != m_New.name)
            return false;
        m_New = other->m_New;
        return true;
    }

    void RemoveComponentPropertyCommand::Apply(Document& document) {
        const auto& properties = document.PropertiesOf(m_Component);
        for (u32 i = 0; i < properties.size(); ++i) {
            if (properties[i].name != m_Name) continue;
            m_Old = properties[i];
            m_Index = i;
            m_Existed = true;
            break;
        }
        document.RemoveComponentProperty(m_Component, m_Name);
    }

    void RemoveComponentPropertyCommand::Undo(Document& document) {
        if (!m_Existed) return;
        // Back where it was: a property list is ordered, and a panel that reshuffles on undo is
        // not the state that was undone to.
        Node* node = document.Find(m_Component);
        if (!node) return;
        const u32 at = std::min(m_Index, static_cast<u32>(node->properties.size()));
        node->properties.insert(node->properties.begin() + at, m_Old);
        document.Touch(m_Component);
    }

    void SetInstancePropertyCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const Node* node = document.Find(m_Instance))
                if (const Value* existing = node->props.Find(InstancePropertyKey(m_Name))) {
                    m_Old = *existing;
                    m_Existed = true;
                }
            m_Captured = true;
        }
        document.SetInstanceProperty(m_Instance, m_Name, m_New);
    }

    void SetInstancePropertyCommand::Undo(Document& document) {
        if (m_Existed) document.SetInstanceProperty(m_Instance, m_Name, m_Old);
        else           document.ClearInstanceProperty(m_Instance, m_Name);
    }

    bool SetInstancePropertyCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const SetInstancePropertyCommand*>(&newer);
        if (!other || other->m_Instance != m_Instance || other->m_Name != m_Name) return false;
        m_New = other->m_New;
        return true;
    }

    void RemoveTokenCommand::Apply(Document& document) {
        if (const Token* existing = document.FindToken(m_Name)) { m_Old = *existing; m_Existed = true; }
        document.RemoveToken(m_Name);
    }

    void RemoveTokenCommand::Undo(Document& document) {
        if (m_Existed) document.SetToken(m_Name, m_Old);
    }

    void RenameTokenCommand::Apply(Document& document) {
        const Token* token = document.FindToken(m_From);
        if (!token || m_From == m_To || document.FindToken(m_To)) return;
        Token moved = *token;
        document.RemoveToken(m_From);
        document.SetToken(m_To, std::move(moved));
        RewriteTokenRefs(document, m_From, m_To);
        m_Applied = true;
    }

    void RenameTokenCommand::Undo(Document& document) {
        if (!m_Applied) return;
        const Token* token = document.FindToken(m_To);
        if (!token) return;
        Token moved = *token;
        document.RemoveToken(m_To);
        document.SetToken(m_From, std::move(moved));
        RewriteTokenRefs(document, m_To, m_From);
    }

    void CloneCommand::Apply(Document& document) {
        if (m_Saved.empty()) {
            m_Created = CloneSubtree(document, m_Source, m_Parent, m_Index);
            if (!m_Created.Valid()) return;
            for (Uuid id : document.Subtree(m_Created))
                if (const Node* node = document.Find(id)) m_Saved.push_back(*node);
            return;
        }
        // Redo: the same nodes, the same ids, in the same order — parents before children, which
        // is the order Subtree collected them in.
        for (const Node& node : m_Saved) {
            Node copy = node;
            copy.children.clear();
            document.InsertNode(std::move(copy), node.id == m_Created ? m_Index : UINT32_MAX);
        }
        for (const Node& node : m_Saved)
            if (Node* live = document.Find(node.id)) live->children = node.children;
    }

    void CloneCommand::Undo(Document& document) {
        if (m_Created.Valid()) document.DeleteNode(m_Created);
    }

    void AddAssetCommand::Apply(Document& document) {
        // The id is minted once and reused on redo: a node that referred to this asset before the
        // undo has to find the same asset after the redo.
        m_Id = document.AddAsset(m_Name, m_Path, m_Id);
    }

    void AddAssetCommand::Undo(Document& document) { document.RemoveAsset(m_Id); }

    void RemoveAssetCommand::Apply(Document& document) {
        if (!m_Captured) {
            if (const Document::Asset* asset = document.FindAsset(m_Id)) {
                m_Name = asset->name;
                m_Path = asset->path;
                m_Captured = true;
            }
        }
        document.RemoveAsset(m_Id);
    }

    void RemoveAssetCommand::Undo(Document& document) {
        if (m_Captured) document.AddAsset(m_Name, m_Path, m_Id);
    }

    void SetStartScreenCommand::Apply(Document& document) {
        if (!m_Captured) { m_Old = document.StartScreen(); m_Captured = true; }
        document.SetStartScreen(m_New);
    }

    void SetStartScreenCommand::Undo(Document& document) { document.SetStartScreen(m_Old); }

    void SetThemeCommand::Apply(Document& document) {
        if (!m_Captured) { m_Old = document.ActiveTheme(); m_Captured = true; }
        document.SetTheme(m_New);
    }

    void SetThemeCommand::Undo(Document& document) { document.SetTheme(m_Old); }

    bool RenameCommand::Coalesce(const Command& newer) {
        const auto* other = dynamic_cast<const RenameCommand*>(&newer);
        if (!other || other->m_Node != m_Node) return false;
        m_New = other->m_New;
        return true;
    }

    // ---------------------------------------------------------------- Composite

    void CompositeCommand::Apply(Document& document) {
        for (auto& command : m_Commands) command->Apply(document);
    }

    void CompositeCommand::Undo(Document& document) {
        // Reverse order: a delete-then-create pair must be undone create-first.
        for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it) (*it)->Undo(document);
    }

    // ---------------------------------------------------------------- Stack

    void CommandStack::Execute(Document& document, Scope<Command> command) {
        command->Apply(document);

        if (m_Transaction) { m_Transaction->Add(std::move(command)); return; }

        if (m_CoalesceInto && m_CoalesceInto->Coalesce(*command)) {
            // Absorbed into the previous entry: the redo stack is still invalidated, because the
            // document has moved on from wherever redo would have taken it.
            m_Redo.clear();
            return;
        }

        m_CoalesceInto = command.get();
        m_Undo.push_back(std::move(command));
        m_Redo.clear();
        Trim();
    }

    bool CommandStack::Undo(Document& document) {
        if (m_Undo.empty()) return false;
        Scope<Command> command = std::move(m_Undo.back());
        m_Undo.pop_back();
        command->Undo(document);
        m_Redo.push_back(std::move(command));
        m_CoalesceInto = nullptr;
        return true;
    }

    bool CommandStack::Redo(Document& document) {
        if (m_Redo.empty()) return false;
        Scope<Command> command = std::move(m_Redo.back());
        m_Redo.pop_back();
        command->Apply(document);
        m_Undo.push_back(std::move(command));
        m_CoalesceInto = nullptr;
        return true;
    }

    std::string_view CommandStack::UndoName() const {
        return m_Undo.empty() ? std::string_view{} : m_Undo.back()->Name();
    }

    std::string_view CommandStack::RedoName() const {
        return m_Redo.empty() ? std::string_view{} : m_Redo.back()->Name();
    }

    void CommandStack::BeginTransaction(std::string name) {
        if (m_Transaction) {
            VAE_CORE_WARN("nested transaction '{}' ignored", name);
            return;
        }
        m_Transaction = CreateScope<CompositeCommand>(std::move(name));
        m_CoalesceInto = nullptr;
    }

    void CommandStack::EndTransaction(Document&) {
        if (!m_Transaction) return;
        if (m_Transaction->Empty()) { m_Transaction.reset(); return; }

        m_Undo.push_back(std::move(m_Transaction));
        m_Transaction.reset();
        m_Redo.clear();
        m_CoalesceInto = nullptr;
        Trim();
    }

    void CommandStack::Clear() {
        m_Undo.clear();
        m_Redo.clear();
        m_Transaction.reset();
        m_CoalesceInto = nullptr;
    }

    void CommandStack::Trim() {
        if (m_Undo.size() <= m_Limit) return;
        const std::size_t excess = m_Undo.size() - m_Limit;
        m_Undo.erase(m_Undo.begin(), m_Undo.begin() + static_cast<std::ptrdiff_t>(excess));
    }

}
