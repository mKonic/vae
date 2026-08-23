#pragma once

#include "vae/doc/Document.h"

#include <chrono>
#include <optional>
#include <string>

namespace vae::doc {

    // Every mutation goes through a Command. This is load-bearing from the first commit rather than
    // bolted on later: Studio, hot reload and the codegen all observe the same change stream, and
    // retrofitting undo onto direct mutation means auditing every call site.
    class Command {
    public:
        virtual ~Command() = default;

        virtual void Apply(Document& document) = 0;
        virtual void Undo(Document& document) = 0;
        virtual std::string_view Name() const = 0;

        // Absorb a newer command of the same kind targeting the same thing, so a drag is one undo
        // entry rather than one per mouse-move. Returns false when they must stay separate.
        virtual bool Coalesce(const Command& newer) { (void)newer; return false; }
    };

    class SetPropCommand final : public Command {
    public:
        SetPropCommand(Uuid node, Prop prop, Value value)
            : m_Node(node), m_Prop(prop), m_New(std::move(value)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set property"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid  m_Node;
        Prop  m_Prop;
        Value m_New;
        Value m_Old;
        bool  m_Captured = false;
    };

    class SetLayoutCommand final : public Command {
    public:
        SetLayoutCommand(Uuid node, layout::LayoutStyle style)
            : m_Node(node), m_New(style) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set layout"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid m_Node;
        layout::LayoutStyle m_New{};
        layout::LayoutStyle m_Old{};
        bool m_Captured = false;
    };

    // Editing an instance is not editing its component. The write lands in the instance's override
    // table keyed by the node inside the master, which is what lets one card say something
    // different without every other card changing with it.
    class SetOverrideCommand final : public Command {
    public:
        SetOverrideCommand(Uuid instance, Uuid nodeInComponent, Prop prop, Value value)
            : m_Instance(instance), m_Node(nodeInComponent), m_Prop(prop), m_New(std::move(value)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set override"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid  m_Instance, m_Node;
        Prop  m_Prop;
        Value m_New, m_Old;
        bool  m_Captured = false;
        bool  m_HadOld = false;
    };

    // The same two edits, filed under a string key: the state overlays ("hovered:fill") that are
    // how a widget says what it looks like while the pointer is on it.
    class SetKeyedPropCommand final : public Command {
    public:
        SetKeyedPropCommand(Uuid node, std::string key, Value value)
            : m_Node(node), m_Key(std::move(key)), m_New(std::move(value)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set property"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid m_Node;
        std::string m_Key;
        Value m_New, m_Old;
        bool m_Captured = false;
    };

    class SetKeyedOverrideCommand final : public Command {
    public:
        SetKeyedOverrideCommand(Uuid instance, Uuid nodeInComponent, std::string key, Value value)
            : m_Instance(instance), m_Node(nodeInComponent), m_Key(std::move(key)),
              m_New(std::move(value)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set override"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid m_Instance, m_Node;
        std::string m_Key;
        Value m_New, m_Old;
        bool m_Captured = false;
        bool m_HadOld = false;
    };

    class CreateNodeCommand final : public Command {
    public:
        CreateNodeCommand(NodeKind kind, Uuid parent, std::string name)
            : m_Kind(kind), m_Parent(parent), m_Name(std::move(name)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Create node"; }
        Uuid Created() const { return m_Created; }

    private:
        NodeKind m_Kind;
        Uuid m_Parent;
        std::string m_Name;
        Uuid m_Created = Uuid::Invalid();
    };

    // Placing a widget from the library is the most-used gesture in the editor, and it was the one
    // edit undo did not cover: CreateInstance was called straight on the document, so Ctrl+Z rolled
    // back the position the instance was given and left the instance behind.
    class CreateInstanceCommand final : public Command {
    public:
        CreateInstanceCommand(Uuid component, Uuid parent, std::string name = {})
            : m_Component(component), m_Parent(parent), m_Name(std::move(name)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Place"; }
        Uuid Created() const { return m_Created; }

    private:
        Uuid m_Component;
        Uuid m_Parent;
        std::string m_Name;
        Uuid m_Created = Uuid::Invalid();
        // Redo has to bring back the same node, overrides and all: anything that referenced it —
        // a script path, a selection, another command on the stack — resolves by id.
        std::optional<Node> m_Snapshot;
    };

    class DeleteNodeCommand final : public Command {
    public:
        explicit DeleteNodeCommand(Uuid node) : m_Node(node) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Delete node"; }

    private:
        Uuid m_Node;
        // The whole subtree, parents first, plus where the root sat. Undo has to restore identity
        // and position exactly, or every reference to a deleted node breaks on undo.
        std::vector<Node> m_Removed;
        u32 m_Index = 0;
    };

    class ReparentCommand final : public Command {
    public:
        ReparentCommand(Uuid node, Uuid parent, u32 index)
            : m_Node(node), m_NewParent(parent), m_NewIndex(index) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Move node"; }

    private:
        Uuid m_Node, m_NewParent, m_OldParent;
        u32  m_NewIndex = 0, m_OldIndex = 0;
        bool m_Captured = false;
    };

    class RenameCommand final : public Command {
    public:
        RenameCommand(Uuid node, std::string name) : m_Node(node), m_New(std::move(name)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Rename"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid m_Node;
        std::string m_New, m_Old;
        bool m_Captured = false;
    };

    // Bringing a picture into the project, and taking one back out. Both are edits like any other
    // — a designer who imports the wrong file expects Ctrl+Z to answer, and one who deletes an
    // asset that six nodes point at expects it back with its id intact, or those nodes are drawing
    // nothing forever.
    class AddAssetCommand final : public Command {
    public:
        AddAssetCommand(std::string name, std::string path) : m_Name(std::move(name)),
                                                              m_Path(std::move(path)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Add asset"; }
        Uuid Created() const { return m_Id; }

    private:
        std::string m_Name, m_Path;
        Uuid m_Id = Uuid::Invalid();
    };

    class RemoveAssetCommand final : public Command {
    public:
        explicit RemoveAssetCommand(Uuid id) : m_Id(id) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Remove asset"; }

    private:
        Uuid m_Id;
        std::string m_Name, m_Path;
        bool m_Captured = false;
    };

    // Which screen the app opens on, and which theme it opens in. Both are properties of the
    // design rather than of the editor, so both are written into the file — and anything written
    // into the file has to be undoable.
    class SetStartScreenCommand final : public Command {
    public:
        explicit SetStartScreenCommand(Uuid screen) : m_New(screen) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Start screen"; }

    private:
        Uuid m_New, m_Old;
        bool m_Captured = false;
    };

    class SetThemeCommand final : public Command {
    public:
        explicit SetThemeCommand(Theme theme) : m_New(theme) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Theme"; }

    private:
        Theme m_New, m_Old = Theme::Dark;
        bool m_Captured = false;
    };

    // A deep copy of a subtree, as one undoable step. Duplicate and Paste are both this: the
    // clone is made once and, from then on, undo deletes it and redo puts the same ids back — so
    // anything that referred to the copy still refers to it after a round trip through history.
    class CloneCommand final : public Command {
    public:
        CloneCommand(Uuid source, Uuid parent, u32 index = UINT32_MAX)
            : m_Source(source), m_Parent(parent), m_Index(index) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Duplicate"; }
        Uuid Created() const { return m_Created; }

    private:
        Uuid m_Source, m_Parent, m_Created = Uuid::Invalid();
        u32  m_Index = UINT32_MAX;
        // The whole copied subtree, so redo restores it node for node with the ids it had rather
        // than cloning again and minting new ones.
        std::vector<Node> m_Saved;
    };

    // A token added, changed, or taken away. The palette is part of the document, so editing it is
    // an edit like any other.
    class SetTokenCommand final : public Command {
    public:
        SetTokenCommand(std::string name, Token token)
            : m_Name(std::move(name)), m_New(std::move(token)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set token"; }
        bool Coalesce(const Command& newer) override;

    private:
        std::string m_Name;
        Token m_New, m_Old;
        bool m_Existed = false, m_Captured = false;
    };

    // --- component properties ---------------------------------------------------------------

    // Declaring or editing one of a component's properties. Undoable like everything else, because
    // renaming a property that instances already answer is exactly the edit somebody wants back.
    class SetComponentPropertyCommand final : public Command {
    public:
        SetComponentPropertyCommand(Uuid component, ComponentProperty property)
            : m_Component(component), m_New(std::move(property)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set component property"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid m_Component;
        ComponentProperty m_New, m_Old;
        bool m_Existed = false, m_Captured = false;
    };

    class RemoveComponentPropertyCommand final : public Command {
    public:
        RemoveComponentPropertyCommand(Uuid component, std::string name)
            : m_Component(component), m_Name(std::move(name)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Remove component property"; }

    private:
        Uuid m_Component;
        std::string m_Name;
        ComponentProperty m_Old;
        u32 m_Index = 0;
        bool m_Existed = false;
    };

    // One instance's answer to one of them.
    class SetInstancePropertyCommand final : public Command {
    public:
        SetInstancePropertyCommand(Uuid instance, std::string name, Value value)
            : m_Instance(instance), m_Name(std::move(name)), m_New(std::move(value)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Set property"; }
        bool Coalesce(const Command& newer) override;

    private:
        Uuid m_Instance;
        std::string m_Name;
        Value m_New, m_Old;
        bool m_Existed = false, m_Captured = false;
    };

    class RemoveTokenCommand final : public Command {
    public:
        explicit RemoveTokenCommand(std::string name) : m_Name(std::move(name)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Remove token"; }

    private:
        std::string m_Name;
        Token m_Old;
        bool m_Existed = false;
    };

    // Renaming a token rewrites every property that referred to it. A rename that left the
    // references behind would be a rename that silently unstyles half the document.
    class RenameTokenCommand final : public Command {
    public:
        RenameTokenCommand(std::string from, std::string to)
            : m_From(std::move(from)), m_To(std::move(to)) {}

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return "Rename token"; }

    private:
        std::string m_From, m_To;
        bool m_Applied = false;
    };

    // Several commands that undo and redo as one. Built by BeginTransaction/EndTransaction.
    class CompositeCommand final : public Command {
    public:
        explicit CompositeCommand(std::string name) : m_Name(std::move(name)) {}

        void Add(Scope<Command> command) { m_Commands.push_back(std::move(command)); }
        bool Empty() const { return m_Commands.empty(); }
        std::size_t Size() const { return m_Commands.size(); }

        void Apply(Document& document) override;
        void Undo(Document& document) override;
        std::string_view Name() const override { return m_Name; }

    private:
        std::string m_Name;
        std::vector<Scope<Command>> m_Commands;
    };

    class CommandStack {
    public:
        // Applies the command and pushes it, unless the previous one absorbed it.
        void Execute(Document& document, Scope<Command> command);

        bool Undo(Document& document);
        bool Redo(Document& document);
        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        std::string_view UndoName() const;
        std::string_view RedoName() const;

        // Ends the current coalescing run — call it on mouse-up so the next drag is its own entry.
        void Break() { m_CoalesceInto = nullptr; }

        void BeginTransaction(std::string name);
        void EndTransaction(Document& document);
        bool InTransaction() const { return m_Transaction != nullptr; }

        void Clear();
        std::size_t UndoDepth() const { return m_Undo.size(); }
        std::size_t RedoDepth() const { return m_Redo.size(); }

        // A finite history: a design session can run for hours and every mouse-drag is an entry.
        void SetLimit(std::size_t limit) { m_Limit = limit; Trim(); }

    private:
        void Trim();

        std::vector<Scope<Command>> m_Undo;
        std::vector<Scope<Command>> m_Redo;
        Command* m_CoalesceInto = nullptr;
        Scope<CompositeCommand> m_Transaction;
        std::size_t m_Limit = 500;
    };

}
