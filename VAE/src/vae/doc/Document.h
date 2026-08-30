#pragma once

#include "vae/doc/Node.h"

#include <functional>
#include <unordered_map>

namespace vae::doc {

    // A named design token with light and dark values. Two variants rather than N themes because
    // that is the shape every design system actually ships, and a general theme map would make
    // every lookup a two-level miss for no benefit yet.
    struct Token {
        Value light;
        Value dark;
        std::string description;
        bool operator==(const Token&) const = default;
    };

    enum class Theme : u8 { Light, Dark };

    // A width a design answers to, by name.
    //
    // `upTo` is a maximum, the way a CSS media query is: `compact` applies while the box is 600
    // wide or less. More than one can be true at once — at 500 both `compact` and `medium` apply —
    // and the narrowest wins, because it is the more specific statement about the same box.
    //
    // A node answers with an overlay named after the breakpoint: `compact:axis` in memory,
    // `compact.axis` on disk. That is the shape `hovered:fill` and `tone=danger:fill` already use,
    // because it is the same idea — a property whose value depends on a condition the node did not
    // choose.
    struct Breakpoint {
        std::string name;
        f32 upTo = 0.0f;
        bool operator==(const Breakpoint&) const = default;
    };

    // What a project gets if it says nothing. Not a law: a document that declares its own replaces
    // these outright, and the writer only puts them in the file when they differ.
    const std::vector<Breakpoint>& DefaultBreakpoints();

    // The rows an app hands to a repeated container. A repeat on its own is one node drawn N
    // times, which is a placeholder; with rows behind it every copy draws a row, which is what a
    // list of messages, of channels or of people actually is.
    //
    // Columns are named because the template names the part of a row it draws — "author", "body".
    // Positions would be a second thing to keep in step, and they break the moment a column moves.
    struct RowTable {
        std::vector<std::string> columns;
        std::vector<std::string> cells;      // row-major, columns.size() wide

        u32  Count() const;
        i32  ColumnOf(std::string_view name) const;
        // Empty when the row or the column is not there: a template bound to a column the data
        // does not carry draws nothing, which is what an absent value means.
        std::string_view Cell(u32 row, std::string_view column) const;
        std::string_view Cell(u32 row, u32 column) const;
        bool operator==(const RowTable&) const = default;
    };

    // A row table written as text, which is how a designer types sample rows and how Prop::Sample
    // stores them. The first non-empty line names the columns, every line after it is a row, and
    // cells are separated by `|` and trimmed:
    //
    //     author | body      | tint
    //     Ada    | Hello     | accent
    //     Grace  | Hi there  | success
    //
    // A short row pads with empties and a long one drops the extra, so a half-typed table still
    // draws rather than refusing to.
    RowTable ParseRowText(std::string_view text);
    // The same table back as text, so what the designer typed survives a round trip through the
    // inspector rather than being reformatted under the cursor.
    std::string RowText(const RowTable& rows);

    // Where a repeated container's rows come from. The document holds no data of its own — this is
    // the runtime lending its rows for the length of one flatten.
    using RowLookup = std::function<const RowTable*(Uuid node, Uuid instance)>;

    // How much of a repeated container is worth building, and how much room the rest of it takes.
    //
    // A list of fifty thousand rows inside a box that shows thirty needs thirty copies, not fifty
    // thousand — but it still has to be fifty thousand rows tall, or the scrollbar lies and the
    // scroll offset means nothing. So the copies outside the window are represented by the space
    // they would have taken: one empty node above the window and one below, sized along the
    // container's own axis.
    //
    // Nodes rather than padding, which was the first attempt: a repeated container is very often
    // the scroller itself, and padding on a scroller is inside its box — it makes the viewport
    // smaller instead of the content taller, and the content size a scrollbar is drawn from is
    // measured off the children. A spacer is a child, so it is right in both shapes.
    struct RowWindow {
        u32 first = 0;      // the first copy to build
        u32 count = 0;      // how many; zero means all of them, which is what a short list wants
        // The extents of the two spacers, gaps already taken off — the caller knows the gap and
        // this keeps the arithmetic in the one place that measured the rows.
        f32 before = 0.0f;
        f32 after = 0.0f;
        // Whether the answer is "all of them, because nothing clips this" or "all of them, because
        // nothing has been measured yet". The first is worth a warning at fifty thousand rows; the
        // second is just the first frame, and happens to every list before it has been laid out.
        bool measured = false;
    };

    // Asked once per repeated container, with how many copies it turned out to have. Absent — on
    // the designer's canvas, in a test, on the first frame before anything has been measured —
    // every copy is built, which is exactly what it used to do.
    using WindowLookup = std::function<RowWindow(Uuid node, Uuid instance, u32 total)>;

    // The design document: a flat map of nodes addressed by Uuid, plus tokens.
    //
    // Flat and id-addressed rather than a pointer tree, because every other system needs stable
    // references into it — the command stack records ids, overrides key on ids, the renderer caches
    // by id, and a selection survives its node being reparented.
    class Document {
    public:
        Document();

        // --- structure -------------------------------------------------------------------------
        Uuid CreateNode(NodeKind kind, Uuid parent = Uuid::Invalid(), std::string name = {});
        // Inserts an already-built node, preserving its id. Used by undo, paste and load.
        void InsertNode(Node node, u32 index = UINT32_MAX);

        // While a scope is open, CreateNode mints ids from the scope name and a counter instead of
        // at random. Only the standard widget catalog uses this: it is rebuilt from code on every
        // load rather than stored in the file, so the same widget has to come back with the same
        // ids or every override an instance recorded against it would go stale. Scoping per
        // component rather than over the whole catalog means adding a widget cannot disturb the
        // ids of the ones already shipped.
        void PushIdScope(std::string scope);
        void PopIdScope();
        void DeleteNode(Uuid id);                       // recursive
        void Reparent(Uuid id, Uuid newParent, u32 index = UINT32_MAX);
        void Reorder(Uuid id, u32 index);

        Node* Find(Uuid id);
        const Node* Find(Uuid id) const;
        bool Contains(Uuid id) const { return m_Nodes.contains(id); }

        const std::vector<Uuid>& Roots() const { return m_Roots; }
        std::size_t NodeCount() const { return m_Nodes.size(); }
        u32 IndexInParent(Uuid id) const;
        // Every node in the subtree, parents before children.
        std::vector<Uuid> Subtree(Uuid root) const;
        // Every node in the document, in no particular order. For the passes that genuinely have
        // to touch all of them — renaming a token rewrites every property that named it.
        std::vector<Uuid> AllNodes() const;
        bool IsAncestor(Uuid ancestor, Uuid descendant) const;

        // --- properties ------------------------------------------------------------------------
        void SetProp(Uuid id, Prop prop, Value value);
        // A property the enum does not name: the state overlays ("hovered:fill") the widget library
        // writes, and anything a project invents for itself.
        void SetProp(Uuid id, std::string key, Value value);
        // Announce that a node changed in a way SetProp did not cover (layout, name, flags).
        void Touch(Uuid id);
        Value GetProp(Uuid id, Prop prop) const;
        // Resolves tokens against the active theme; returns the literal a renderer should use.
        Value ResolveValue(const Value& value) const;

        // --- component properties ----------------------------------------------------------
        // What an instance picked for one of its component's properties, or the component's
        // default when it picked nothing. Unset when the component has no such property.
        Value InstanceProperty(Uuid instance, std::string_view name) const;
        void  SetInstanceProperty(Uuid instance, std::string_view name, Value value);
        void  ClearInstanceProperty(Uuid instance, std::string_view name);
        // The properties an instance can be given, which are its component's.
        const std::vector<ComponentProperty>& PropertiesOf(Uuid component) const;
        const ComponentProperty* FindProperty(Uuid component, std::string_view name) const;
        void SetComponentProperty(Uuid component, ComponentProperty property);
        void RemoveComponentProperty(Uuid component, std::string_view name);
        // Answers the bindings that name a component property and applies the variant overlays for
        // the options an instance picked. Public because the runtime resolves the same way.
        void ApplyComponentProperties(Uuid component, Uuid instance, PropBag& bag) const;

        // --- tokens ----------------------------------------------------------------------------
        // --- breakpoints ------------------------------------------------------------------------
        // Kept sorted widest-first, which is the order overlays apply in: every matching breakpoint
        // is applied and the narrowest lands last, so it wins.
        const std::vector<Breakpoint>& Breakpoints() const { return m_Breakpoints; }
        void SetBreakpoints(std::vector<Breakpoint> breakpoints);
        // Which breakpoints a box this wide is inside, as a bitmask of indices into Breakpoints().
        u32 BreakpointsAt(f32 width) const;
        // The one that decides, or empty — the narrowest that matches, which is what a designer
        // means by "which breakpoint am I looking at".
        std::string_view NarrowestAt(f32 width) const;

        void SetToken(const std::string& name, Token token);
        void RemoveToken(const std::string& name);
        const Token* FindToken(std::string_view name) const;
        const std::map<std::string, Token>& Tokens() const { return m_Tokens; }
        void SetTheme(Theme theme) { m_Theme = theme; }

        // Which screen an app opens on. A document fact, so the player, an exported build and the
        // Studio's preview all agree without anyone passing it around — and so a designer can say
        // "start here" instead of reordering the file.
        void SetStartScreen(Uuid screen) { m_StartScreen = screen; }
        // Falls back to the first screen in the document, which is the answer before anyone has
        // chosen and after the chosen one has been deleted.
        Uuid StartScreen() const;
        // What was actually chosen, with no falling back to the first screen. A file records the
        // choice somebody made, and a document holding one screen has not made one — writing the
        // fallback out would turn "no opinion" into "this one", which is wrong the moment that
        // document is one screen of a project that has several.
        Uuid ChosenStartScreen() const { return m_StartScreen; }
        // Every screen, in document order.
        std::vector<Uuid> Screens() const;
        ScreenKind KindOf(Uuid screen) const;
        Theme ActiveTheme() const { return m_Theme; }

        // --- assets ------------------------------------------------------------------------------
        // What the project has pictures of. Kept by id with a path relative to the project folder,
        // so a node refers to "this image" and not to a place on one person's disk — moving or
        // renaming a project cannot break every image in it.
        struct Asset {
            Uuid id;
            std::string name;
            std::string path;          // relative to the project folder
            bool operator==(const Asset&) const = default;
        };
        // Returns the id, which is generated when `id` is invalid and preserved when it is not
        // (load, undo and paste all have to keep the one they already have).
        Uuid AddAsset(std::string name, std::string path, Uuid id = Uuid::Invalid());
        void RemoveAsset(Uuid id);
        const Asset* FindAsset(Uuid id) const;
        // By the name the Assets panel shows, which is what a script says and what a row cell
        // spells: "avatar-ada" is nameable in data, and the Uuid behind it is not.
        const Asset* FindAssetNamed(std::string_view name) const;
        const std::vector<Asset>& Assets() const { return m_Assets; }

        // --- components ------------------------------------------------------------------------
        // Turns a subtree into a reusable component in place, returning the component's id.
        Uuid MakeComponent(Uuid subtreeRoot, std::string name);
        Uuid CreateInstance(Uuid componentId, Uuid parent);
        void SetOverride(Uuid instance, Uuid nodeInComponent, Prop prop, Value value);
        void ClearOverride(Uuid instance, Uuid nodeInComponent, Prop prop);
        // The same pair for a string-keyed property — the state overlays ("hovered:fill") a widget
        // is styled with. An instance that cannot override those cannot be restyled at all.
        void SetOverride(Uuid instance, Uuid nodeInComponent, std::string key, Value value);
        void ClearOverride(Uuid instance, Uuid nodeInComponent, std::string_view key);

        // The properties a node inside an instance should actually render with:
        // instance override > component node's own props.
        PropBag ResolvedProps(Uuid instance, Uuid nodeInComponent) const;
        // The same question for a node nested several components deep. `chain` is every instance it
        // sits inside, outermost first. They resolve outward-in — an override written on the
        // instance that is actually on the screen beats one the component's author baked in — which
        // is what makes two copies of a component two things rather than one.
        PropBag ResolvedProps(const std::vector<Uuid>& chain, Uuid node) const;

        // Expands instances into a concrete tree for rendering/layout. The expansion is
        // throwaway — the document stays the single source of truth.
        struct FlatNode {
            Uuid sourceId;          // the node in the document (inside a component, for instances)
            // Which copy of an instance produced it, or Invalid. Unique per copy on screen, so it
            // is what widget and script state key on — but only the outermost one is a real node.
            Uuid instanceId;
            // The instance whose override table a write should land in, or Invalid. Always the
            // outermost one — the instance a designer can actually select on the screen — so
            // retitling one card's button does not retitle every card's.
            Uuid overrideId;
            // The key that write is filed under: the node this flat node is about, from the point
            // of view of `overrideId`. An instance's own root is keyed by its component, which is
            // the convention every SetOverride call already uses.
            Uuid overrideKey;
            // The authored node this came from — the instance node itself for an instance root,
            // where `sourceId` is the component it points at. What "which component is this?" reads.
            Uuid authoredId;
            u32  parent = UINT32_MAX;
            // One of a repeated container's copies, or inside one. Such a node has no document
            // node of its own to write to — every copy shares the one the designer drew — so what
            // a widget changes about it is runtime state keyed on the copy, not a document edit.
            bool repeated = false;
            // Which copy, counting from zero, or -1 outside one. A click has to be able to say
            // which row it was, and the name of a copy ("Channel 3") is a label, not an index.
            i32 row = -1;
            // The top of a copy, as opposed to something drawn inside one. A row nested in a row
            // has a row number of its own, so "which copy is this" cannot be answered by walking
            // up until the numbers run out.
            bool rowRoot = false;
            NodeKind kind = NodeKind::Frame;
            layout::LayoutStyle layout{};
            PropBag props;
            std::string name;
        };
        // `rows` answers with what a repeated container was handed, if anything. Without it a
        // repeat is still a repeat — it just draws the template's own text N times.
        std::vector<FlatNode> Flatten(Uuid root, const RowLookup& rows = {},
                                      const WindowLookup& windows = {}) const;

        // --- change notification ---------------------------------------------------------------
        // Studio, the renderer and hot reload all watch the same stream, so there is exactly one
        // definition of "the document changed".
        using Observer = std::function<void(Uuid changed)>;
        u32  AddObserver(Observer observer);
        void RemoveObserver(u32 handle);
        u64  Revision() const { return m_Revision; }

        void Clear();

        // The frame inside `component` that an instance's children land in, or Invalid. First one
        // wins: a component has one slot, which is enough for a card, a field or a list row and
        // keeps "where does this go?" from needing an answer.
        Uuid SlotOf(Uuid component) const;

    private:
        void Notify(Uuid changed);
        void DetachFromParent(Uuid id);

        // Content an instance is handing to the component's slot, plus the scope those nodes were
        // authored in — they belong to the page, not to the component, so their overrides and their
        // instance path have to keep resolving against where they were written.
        struct SlotContent {
            const std::vector<Uuid>* children = nullptr;
            std::vector<Uuid> chain;
            Uuid pathContext = Uuid::Invalid();
        };
        // Which row of which table a node is being flattened for. Null everywhere except inside
        // a repeated copy, which is the only place a field binding means anything.
        struct RowBinding {
            const RowTable* table = nullptr;
            u32 row = 0;
        };
        void FlattenInto(std::vector<FlatNode>& out, Uuid id, u32 parent,
                         std::vector<Uuid>& chain, Uuid pathContext, u32 depth,
                         const SlotContent* slot = nullptr, const RowLookup* rows = nullptr,
                         const RowBinding* row = nullptr,
                         const WindowLookup* windows = nullptr) const;

        std::string m_IdScope;      // empty except while the standard library is being built
        u32 m_IdCounter = 0;

        std::unordered_map<Uuid, Node> m_Nodes;
        std::vector<Uuid> m_Roots;
        std::map<std::string, Token> m_Tokens;
        std::vector<Breakpoint> m_Breakpoints;
        std::vector<Asset> m_Assets;
        Theme m_Theme = Theme::Dark;
        Uuid m_StartScreen = Uuid::Invalid();

        std::vector<std::pair<u32, Observer>> m_Observers;
        u32 m_NextObserver = 1;
        u64 m_Revision = 0;
    };

    // A deep copy of a subtree under a new parent, with fresh ids throughout. What Duplicate and
    // Paste are both made of — copying a frame without its children is copying an empty frame,
    // which is what Duplicate used to do.
    //
    // Overrides are NOT remapped, deliberately: an instance's overrides are keyed by the id of a
    // node inside the *component*, and the component is not what is being copied. A property that
    // points at another node in the same subtree (a NodeRef) is remapped, because that reference
    // means "this one" rather than "that one over there".
    Uuid CloneSubtree(Document& document, Uuid source, Uuid parent, u32 index = UINT32_MAX);

    // The same copy, across documents — the clipboard, in both directions.
    //
    // `freshIds` is the whole difference between the two directions. Writing to the clipboard keeps
    // the ids, because an instance in there points at a component in there and its overrides are
    // keyed by the ids of nodes inside it. Reading back mints new ones, because pasting twice must
    // not produce two nodes claiming the same id.
    Uuid CopySubtreeInto(const Document& from, Uuid root, Document& into, Uuid parent,
                         bool freshIds, u32 index = UINT32_MAX);

}
