#pragma once

#include "vae/doc/Value.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vae::doc {

    // A blueprint is component logic drawn instead of typed: Unreal's Blueprint model, which is the
    // one visual-scripting design that has survived contact with real applications.
    //
    // Two kinds of wire, and the split is the whole idea:
    //   - an **execution** wire says what happens next. It is white, it carries no value, and one
    //     output drives one input, because "next" is singular.
    //   - a **data** wire says where a value comes from. It is typed and coloured by its type, one
    //     input takes one source, and one output may feed many.
    //
    // Everything else follows from it. A node with execution pins is a statement and runs when the
    // wire reaches it; a node without them is an expression, and it is evaluated when something
    // asks for its value. That is why `Branch` has two outputs and `+` has none.
    //
    // A blueprint belongs to the screen or component it drives, and is stored inside that document —
    // see D19. There is no separate blueprint file, for the same reason there is no separate style
    // file: what a file holds is what is inside it.

    // What a pin carries. Exec is not a value, which is exactly why it is not a ValueType — a wire
    // that carries "now" and a wire that carries 3 are different wires, and letting one be plugged
    // into the other is the first bug a blueprint editor has to refuse.
    enum class PinType : u8 {
        Exec = 0,
        Bool,
        Number,
        Text,
        Colour,
        // A pin that takes whatever it is handed and passes the same type along. Only the nodes
        // that genuinely do not care what they are moving — Select, and a variable of a type the
        // blueprint decides — and never as a way of avoiding a decision about the others.
        Any,
    };

    const char* PinTypeName(PinType type);
    std::optional<PinType> PinTypeFromName(std::string_view name);
    // The value a pin of this type holds, or Unset for Exec, which holds none.
    ValueType ValueTypeOf(PinType type);
    PinType PinTypeOf(ValueType type);
    // Whether a wire from `from` may end at `to`. Number into Text is allowed and converts, which
    // is what every language does with `"count: " + n`; Exec into anything else never is.
    bool PinsCompatible(PinType from, PinType to);

    // What a node type is for, which decides its colour and where the palette files it. Unreal's
    // categories, because a designer who has seen one Blueprint already knows them: red is an
    // event, blue is something that happens, green is something that is worked out.
    enum class BlueprintCategory : u8 {
        Event,      // an entry point: something happened
        Flow,       // branch, loop, sequence, delay
        Variable,   // get and set
        Widget,     // the component's own tree
        App,        // navigation, timers, logging, what the app does
        Data,       // arithmetic, comparison, text — pure, always
        Service,    // the store, files, the network, sound
        Count
    };

    const char* BlueprintCategoryName(BlueprintCategory category);

    struct PinSpec {
        std::string_view name;
        PinType type = PinType::Exec;
        // A literal a pin starts with when nothing is wired to it. Written as text and parsed
        // against the pin's type, so the table stays a table.
        std::string_view literal;
        // Only ever a literal — no wire may end here. An event's filter is the case: which widget
        // an On Clicked answers to is a fact about the blueprint, known before it runs, and a wire
        // would make it a question asked after the click already happened.
        bool fixed = false;
    };

    // One kind of node, declared once and read by all three things that care: the editor draws its
    // pins from this, the compiler checks a blueprint against it, and the C++ emitter writes the call
    // it names. Three copies of this table is exactly the bug the layout-field table was written
    // to end — one of them is always the one nobody updated.
    struct BlueprintNodeType {
        std::string_view id;            // "flow.branch" — what the file stores
        std::string_view title;         // "Branch" — what the node says
        BlueprintCategory category = BlueprintCategory::Flow;
        // No execution pins: an expression, evaluated when something reads it rather than when a
        // wire reaches it. Unreal calls these pure and draws them green.
        bool pure = false;
        std::vector<PinSpec> inputs;
        std::vector<PinSpec> outputs;
        std::string_view summary;       // one line, for the palette and the tooltip
        // The call in `VaeScript.h` this is, spelled the way a script author would write it. It is
        // documentation and it is what the C++ emitter writes, which is the same thing said twice
        // on purpose: a node that cannot name a real call is a node that invented a capability.
        std::string_view call;
        // A node whose second output picks up later rather than now: Delay is the one, and it is
        // why the runtime can suspend a blueprint at all.
        bool latent = false;
        // Grows a numbered run of its last output pin — Sequence's Then 0, Then 1, Then 2.
        bool variadicOut = false;
    };

    // Every node type there is, in palette order.
    const std::vector<BlueprintNodeType>& BlueprintNodeTypes();
    const BlueprintNodeType* FindBlueprintNodeType(std::string_view id);

    // A value a blueprint holds between events, by name. Stored in the runtime's per-instance state
    // bag rather than in the blueprint, which is what makes a hot reload keep the screen it was on:
    // the bag survives the module being thrown away, and always did.
    struct BlueprintVariable {
        std::string name;
        ValueType type = ValueType::Number;
        Value defaultValue;
        bool operator==(const BlueprintVariable&) const = default;
    };

    struct BlueprintNode {
        u32 id = 0;
        std::string type;               // an id from the table above
        Vec2 position{};
        // The variable a get/set names. Not a pin: a variable node is a *different node* per
        // variable in Unreal too, and a wire here would mean the blueprint could not be checked
        // until it ran.
        std::string target;
        // What an unconnected input pin holds, by pin name. Absent means the type's own default.
        std::map<std::string, Value> literals;
        // Extra numbered outputs for a variadic type. Sequence starts with two Thens.
        u32 extraPins = 0;
        // The note the author stuck on this node, drawn above it. Unreal's node comment.
        std::string comment;
        bool operator==(const BlueprintNode&) const = default;
    };

    struct BlueprintLink {
        u32 id = 0;
        u32 from = 0;                   // node id
        std::string fromPin;            // output pin name
        u32 to = 0;
        std::string toPin;              // input pin name
        bool operator==(const BlueprintLink&) const = default;
    };

    // A box drawn behind a group of nodes, with a title. Pure annotation — it holds nothing and
    // runs nothing — but a blueprint of forty nodes with no regions in it is a wall, and every
    // Blueprint that anyone has to read again has these.
    struct BlueprintComment {
        u32 id = 0;
        std::string text;
        Vec2 position{};
        Vec2 size{ 320.0f, 180.0f };
        bool operator==(const BlueprintComment&) const = default;
    };

    class Blueprint {
    public:
        std::vector<BlueprintNode> nodes;
        std::vector<BlueprintLink> links;
        std::vector<BlueprintVariable> variables;
        std::vector<BlueprintComment> comments;

        bool Empty() const {
            return nodes.empty() && links.empty() && variables.empty() && comments.empty();
        }
        bool operator==(const Blueprint&) const = default;

        // Ids are minted from one counter shared by nodes, links and comments, so nothing that
        // refers to "the thing with id 7" can find two of them. The editor addresses everything by
        // id, and so does every link.
        u32 MintId();
        u32 NextId() const { return m_NextId; }
        // Load and undo both put a blueprint back exactly as it was, which means putting the counter
        // back too — otherwise the next node minted collides with one already in the file.
        void SetNextId(u32 next) { m_NextId = next; }
        // The counter that cannot collide with anything currently in the blueprint. What a reader
        // calls after filling one in from a file that did not record it.
        void RecomputeNextId();

        BlueprintNode* Find(u32 id);
        const BlueprintNode* Find(u32 id) const;
        BlueprintComment* FindComment(u32 id);

        u32  AddNode(BlueprintNode node);       // returns the id, minting one when it has none
        void RemoveNode(u32 id);            // and every link touching it
        u32  AddLink(BlueprintLink link);
        void RemoveLink(u32 id);
        // A data input takes one wire and an exec output drives one target, so connecting either
        // replaces what was there. Both directions of that rule live here rather than in the
        // editor, because the compiler has to be able to trust it about a file it did not draw.
        void DisplaceAt(u32 node, std::string_view pin, bool isInput, bool exec);

        const BlueprintVariable* FindVariable(std::string_view name) const;
        void SetVariable(BlueprintVariable variable);
        void RemoveVariable(std::string_view name);
        // Renames it and every get/set that names it, which is the only version of this operation
        // that leaves a blueprint that still runs.
        void RenameVariable(std::string_view from, std::string_view to);

        // What is wired into this input, or null. One source, always: this is what a data pin
        // taking a single wire buys.
        const BlueprintLink* LinkInto(u32 node, std::string_view pin) const;
        // What this execution output drives, or null.
        const BlueprintLink* LinkOutOf(u32 node, std::string_view pin) const;
        std::vector<const BlueprintLink*> LinksOutOf(u32 node, std::string_view pin) const;

    private:
        u32 m_NextId = 1;
    };

    // The pins a node actually has, which is its type's pins with the blueprint's answers filled in:
    // a variable's type decides a get's output, and a variadic type's extra pins are numbered on.
    // Everything that touches pins goes through this — the editor, the compiler and the emitter —
    // so a node cannot be drawn with pins the compiler does not believe in.
    std::vector<PinSpec> BlueprintInputs(const Blueprint& blueprint, const BlueprintNode& node);
    std::vector<PinSpec> BlueprintOutputs(const Blueprint& blueprint, const BlueprintNode& node);
    // Named because a variadic pin's name is generated: "Then 0", "Then 1".
    std::string SequencePinName(u32 index);

    // What a node calls itself on screen. A variable's get says the variable's name, an event says
    // what it is bound to, and everything else says its type's title.
    std::string BlueprintNodeTitle(const Blueprint& blueprint, const BlueprintNode& node);

    // The literal on a pin: what the node holds, or what its type says it starts with.
    Value BlueprintLiteral(const Blueprint& blueprint, const BlueprintNode& node, const PinSpec& pin);
    // The text form the file stores and the table declares, parsed against a pin type.
    Value ParsePinLiteral(std::string_view text, PinType type);
    std::string PinLiteralText(const Value& value, PinType type);

}
