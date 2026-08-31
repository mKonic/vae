#include "Test.h"

#include "vae/doc/Blueprint.h"
#include "vae/doc/Serializer.h"
#include "vae/script/BlueprintHost.h"
#include "vae/script/BlueprintProgram.h"
#include "vae/script/Runtime.h"
#include "vae/ui/Library.h"
#include "vae/ui/UiHost.h"
#include "vae/ui/Widget.h"

#include <algorithm>
#include <string>

using namespace vae;

namespace {

    // A component with a button called Increment and a text node called Label, driven by a blueprint
    // instead of by a script. Deliberately the same shape as t_script's Scripted fixture: the
    // point of the blueprint host is that it drives the same tree through the same table, so the two
    // suites should be checking the same things about two spellings of one behaviour.
    struct Blueprinted {
        doc::Document document;
        ui::UiHost host;
        script::Runtime runtime;
        script::BlueprintHost* blueprints = nullptr;
        Uuid screen = Uuid::Invalid();
        Uuid component = Uuid::Invalid();
        Uuid instance = Uuid::Invalid();
        Vec2 size{ 400.0f, 300.0f };

        Blueprinted() {
            component = document.CreateNode(doc::NodeKind::Component, Uuid::Invalid(), "Counter");
            doc::Node* master = document.Find(component);
            master->layout.mode = layout::LayoutMode::Stack;
            master->layout.axis = layout::Axis::Column;
            master->layout.width = layout::Size::Px(200.0f);
            master->layout.height = layout::Size::Px(80.0f);

            const Uuid button = document.CreateNode(doc::NodeKind::Frame, component, "Increment");
            document.SetProp(button, doc::Prop::Role,
                             std::string(ui::RoleName(ui::Role::Button)));
            doc::Node* buttonNode = document.Find(button);
            buttonNode->layout.width = layout::Size::Px(120.0f);
            buttonNode->layout.height = layout::Size::Px(32.0f);

            const Uuid label = document.CreateNode(doc::NodeKind::Text, component, "Label");
            document.SetProp(label, doc::Prop::Text, std::string("-"));

            screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Screen");
            doc::Node* screenNode = document.Find(screen);
            screenNode->layout.mode = layout::LayoutMode::Absolute;
            screenNode->layout.width = layout::Size::Px(size.x);
            screenNode->layout.height = layout::Size::Px(size.y);

            instance = document.CreateInstance(component, screen);
            document.Find(instance)->layout.offsetStart = { 20.0f, 20.0f };
            document.Touch(instance);

            host.SetDocument(document, screen);
            runtime.Attach(host, document);
        }

        // Compiles whatever blueprints the document now holds and starts running them.
        void Start() {
            auto host_ = CreateScope<script::BlueprintHost>();
            blueprints = host_.get();
            runtime.AddHost(std::move(host_));
            blueprints->Adopt(document);
            Frame();
        }

        void Frame(f32 dt = 1.0f / 60.0f) {
            runtime.Dispatch(host.TakeActions());
            host.ApplyNavigation();
            host.Update(size, dt);
            runtime.Sync();
            runtime.Update(dt);
        }

        u32 ViewNamed(std::string_view name) const {
            const ui::ViewTree& tree = host.Tree();
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).instanceId == instance && tree.At(i).name == name) return i;
            return ui::ViewTree::kInvalid;
        }

        std::string TextOf(std::string_view name) const {
            const u32 view = ViewNamed(name);
            return view == ui::ViewTree::kInvalid ? std::string{}
                                                  : host.Tree().Str(view, doc::Prop::Text);
        }

        void Click(std::string_view name) {
            const u32 view = ViewNamed(name);
            if (view == ui::ViewTree::kInvalid) return;
            const Vec2 point = host.Tree().Bounds(view).Center();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            Frame();
        }

        std::string StateText(const std::string& key) const {
            const auto* bag = runtime.StateOf(instance);
            if (!bag) return {};
            const auto it = bag->find(key);
            if (it == bag->end()) return {};
            const std::string* text = std::get_if<std::string>(&it->second);
            return text ? *text : std::string();
        }

        double State(const std::string& key) const {
            const auto* bag = runtime.StateOf(instance);
            if (!bag) return 0.0;
            const auto it = bag->find(key);
            if (it == bag->end()) return 0.0;
            const f32* number = std::get_if<f32>(&it->second);
            return number ? *number : 0.0;
        }
    };

    // Small builders, so a test reads as the blueprint it is describing rather than as struct filling.
    // Adds to the event graph unless a canvas is named: every test that does not mention a
    // function is a test about the event graph.
    u32 Add(doc::Blueprint& blueprint, std::string type, std::string target = {},
            doc::BlueprintCanvas* into = nullptr) {
        doc::BlueprintNode node;
        node.type = std::move(type);
        node.target = std::move(target);
        return blueprint.AddNode(into ? *into : blueprint.graph, std::move(node));
    }

    void Wire(doc::Blueprint& blueprint, u32 from, std::string fromPin, u32 to, std::string toPin,
              doc::BlueprintCanvas* into = nullptr) {
        blueprint.AddLink(into ? *into : blueprint.graph,
                          { 0, from, std::move(fromPin), to, std::move(toPin) });
    }

    void Lit(doc::Blueprint& blueprint, u32 node, const std::string& pin, doc::Value value,
             doc::BlueprintCanvas* into = nullptr) {
        doc::BlueprintCanvas& canvas = into ? *into : blueprint.graph;
        if (doc::BlueprintNode* found = canvas.Find(node)) found->literals[pin] = std::move(value);
    }

    bool Says(const std::vector<script::BlueprintProgram::Diagnostic>& diagnostics,
              std::string_view fragment, bool error = true) {
        return std::ranges::any_of(diagnostics, [&](const auto& d) {
            return d.error == error && d.message.find(fragment) != std::string::npos;
        });
    }

    // The blueprint every "does it run" test starts from: click Increment, add one to `count`, show it.
    doc::Blueprint Counter() {
        doc::Blueprint blueprint;
        blueprint.SetVariable({ "count", doc::PinType::Number, 0.0f });

        const u32 clicked = Add(blueprint, "event.clicked");
        Lit(blueprint, clicked, "Node", std::string("Increment"));
        const u32 get = Add(blueprint, "var.get", "count");
        const u32 add = Add(blueprint, "math.add");
        Lit(blueprint, add, "B", 1.0f);
        const u32 set = Add(blueprint, "var.set", "count");
        const u32 show = Add(blueprint, "ui.setText");
        Lit(blueprint, show, "Node", std::string("Label"));

        Wire(blueprint, get, "Value", add, "A");
        Wire(blueprint, add, "Value", set, "Value");
        Wire(blueprint, clicked, "Out", set, "In");
        Wire(blueprint, set, "Out", show, "In");
        Wire(blueprint, set, "Value", show, "Value");
        return blueprint;
    }

}

// ------------------------------------------------------------------------------ the node table

TEST(blueprint, every_node_type_is_well_formed) {
    std::vector<std::string_view> ids;
    for (const doc::BlueprintNodeType& type : doc::BlueprintNodeTypes()) {
        CHECK_MESSAGE(std::ranges::find(ids, type.id) == ids.end(), std::string(type.id));
        ids.push_back(type.id);
        CHECK(!type.title.empty());
        CHECK(!type.summary.empty());
        // Every node names the call it really is. A node that cannot is a node that invented a
        // capability the script API does not have.
        CHECK_MESSAGE(!type.call.empty(), std::string(type.id));

        doc::BlueprintNode node;
        node.type = std::string(type.id);
        doc::Blueprint blueprint;
        for (const std::vector<doc::PinSpec>& side :
             { doc::BlueprintInputs(blueprint, node), doc::BlueprintOutputs(blueprint, node) }) {
            std::vector<std::string_view> names;
            for (const doc::PinSpec& pin : side) {
                CHECK_MESSAGE(std::ranges::find(names, pin.name) == names.end(),
                              std::string(type.id) + " / " + std::string(pin.name));
                names.push_back(pin.name);
                // The codec writes a pin's literal under the pin's own name and its own reserved
                // attributes in lower case. That is only safe while every pin starts with a
                // capital, so it is checked rather than remembered.
                CHECK_MESSAGE(!pin.name.empty() && pin.name[0] >= 'A' && pin.name[0] <= 'Z',
                              std::string(type.id) + " / " + std::string(pin.name));
            }
        }
    }
}

TEST(blueprint, a_pure_node_has_no_execution_pins) {
    for (const doc::BlueprintNodeType& type : doc::BlueprintNodeTypes()) {
        if (!type.pure) continue;
        for (const doc::PinSpec& pin : type.inputs)
            CHECK_MESSAGE(pin.type != doc::PinType::Exec, std::string(type.id));
        for (const doc::PinSpec& pin : type.outputs)
            CHECK_MESSAGE(pin.type != doc::PinType::Exec, std::string(type.id));
    }
}

TEST(blueprint, a_statement_can_be_reached_and_an_event_cannot) {
    for (const doc::BlueprintNodeType& type : doc::BlueprintNodeTypes()) {
        // The three signature nodes have no pins in the table at all: theirs come from the
        // function they belong to, and the test below is the one that checks them.
        if (type.category == doc::BlueprintCategory::Function) continue;
        const bool event = type.category == doc::BlueprintCategory::Event;
        bool hasExecIn = false;
        for (const doc::PinSpec& pin : type.inputs)
            if (pin.type == doc::PinType::Exec) hasExecIn = true;
        if (event || type.pure) CHECK_MESSAGE(!hasExecIn, std::string(type.id));
        else                    CHECK_MESSAGE(hasExecIn, std::string(type.id));
    }
}

TEST(blueprint, a_functions_pins_are_its_signature) {
    doc::Blueprint blueprint;
    doc::BlueprintFunction show;
    show.name = "Show";
    show.params  = { { "count", doc::PinType::Number, 0.0f },
                     { "label", doc::PinType::Text, std::string() } };
    show.returns = { { "text", doc::PinType::Text, std::string() } };
    blueprint.SetFunction(show);

    doc::BlueprintNode entry;  entry.type  = "func.entry";
    doc::BlueprintNode ret;    ret.type    = "func.return";
    doc::BlueprintNode call;   call.type   = "func.call"; call.target = "Show";

    // The entry hands out what the call was given, in the order the signature declares.
    const std::vector<doc::PinSpec> out = doc::BlueprintOutputs(blueprint, entry, "Show");
    CHECK_EQ(out.size(), std::size_t{ 3 });
    CHECK_EQ(std::string(out[0].name), std::string("Out"));
    CHECK(out[0].type == doc::PinType::Exec);
    CHECK_EQ(std::string(out[1].name), std::string("count"));
    CHECK(out[1].type == doc::PinType::Number);

    // The return takes what it hands back.
    const std::vector<doc::PinSpec> in = doc::BlueprintInputs(blueprint, ret, "Show");
    CHECK_EQ(in.size(), std::size_t{ 2 });
    CHECK_EQ(std::string(in[1].name), std::string("text"));

    // And a call is both halves, from the outside.
    CHECK_EQ(doc::BlueprintInputs(blueprint, call).size(), std::size_t{ 3 });
    CHECK_EQ(doc::BlueprintOutputs(blueprint, call).size(), std::size_t{ 2 });

    // A pure function has no execution pins at either end.
    show.pure = true;
    blueprint.SetFunction(show);
    CHECK_EQ(doc::BlueprintOutputs(blueprint, entry, "Show").size(), std::size_t{ 2 });
    CHECK_EQ(doc::BlueprintInputs(blueprint, call).size(), std::size_t{ 2 });
}

TEST(blueprint, execution_and_values_are_different_wires) {
    using doc::PinType;
    CHECK(doc::PinsCompatible(PinType::Exec, PinType::Exec));
    CHECK(!doc::PinsCompatible(PinType::Exec, PinType::Number));
    CHECK(!doc::PinsCompatible(PinType::Number, PinType::Exec));
    // What every language converts silently, and nothing else.
    CHECK(doc::PinsCompatible(PinType::Number, PinType::Text));
    CHECK(doc::PinsCompatible(PinType::Bool, PinType::Text));
    CHECK(doc::PinsCompatible(PinType::Text, PinType::Number));
    CHECK(!doc::PinsCompatible(PinType::Colour, PinType::Number));
    CHECK(!doc::PinsCompatible(PinType::Number, PinType::Colour));
}

TEST(blueprint, one_value_in_and_one_next_out) {
    doc::Blueprint blueprint;
    const u32 a = Add(blueprint, "math.add");
    const u32 b = Add(blueprint, "math.add");
    const u32 c = Add(blueprint, "math.add");
    Wire(blueprint, a, "Value", c, "A");
    // A second wire into the same data input replaces the first: a value has one source.
    blueprint.graph.DisplaceAt(c, "A", true, false);
    Wire(blueprint, b, "Value", c, "A");
    CHECK_EQ(blueprint.graph.links.size(), std::size_t{ 1 });
    CHECK_EQ(blueprint.graph.LinkInto(c, "A")->from, b);

    // A data OUTPUT fans out freely — the same value read in three places.
    Wire(blueprint, a, "Value", c, "B");
    blueprint.graph.DisplaceAt(a, "Value", false, false);
    CHECK_EQ(blueprint.graph.links.size(), std::size_t{ 2 });
}

// ------------------------------------------------------------------------------ the codec

TEST(blueprint, a_graph_survives_a_round_trip_through_markup) {
    Blueprinted fixture;
    doc::Blueprint blueprint = Counter();
    blueprint.graph.comments.push_back({ 0, "counting", { 10.0f, 20.0f }, { 300.0f, 150.0f } });
    fixture.document.SetBlueprint(fixture.component, blueprint);

    const std::string first = doc::Serializer::ToXml(fixture.document, true,
                                                     &ui::StandardLibrary());
    CHECK(first.find("<blueprint>") != std::string::npos);

    doc::Document read;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(first, read, &error, &ui::StandardLibrary()), error);
    const std::string second = doc::Serializer::ToXml(read, true, &ui::StandardLibrary());
    CHECK_MESSAGE(first == second, "re-encoding a blueprint did not produce the same markup");

    // And the blueprint itself came back, not just bytes that look alike.
    const doc::Blueprint* back = nullptr;
    for (const auto& [id, candidate] : read.Blueprints()) { back = &candidate; break; }
    CHECK(back != nullptr);
    if (back) {
        CHECK_EQ(back->graph.nodes.size(), blueprint.graph.nodes.size());
        CHECK_EQ(back->graph.links.size(), blueprint.graph.links.size());
        CHECK_EQ(back->variables.size(), std::size_t{ 1 });
        CHECK_EQ(back->graph.comments.size(), std::size_t{ 1 });
        CHECK_EQ(back->graph.comments.front().text, std::string("counting"));
    }
}

TEST(blueprint, a_literal_the_type_already_says_is_not_written) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    // Seconds defaults to 1 in the table. Saying so again is a byte the reader did not need.
    const u32 toast = Add(blueprint, "app.toast");
    Lit(blueprint, toast, "Seconds", 3.0f);
    fixture.document.SetBlueprint(fixture.component, blueprint);
    const std::string with = doc::Serializer::ToXml(fixture.document, true, &ui::StandardLibrary());
    CHECK(with.find("Seconds") == std::string::npos);

    Lit(blueprint, toast, "Seconds", 9.0f);
    fixture.document.SetBlueprint(fixture.component, blueprint);
    const std::string without = doc::Serializer::ToXml(fixture.document, true,
                                                       &ui::StandardLibrary());
    CHECK(without.find("Seconds=\"9\"") != std::string::npos);
}

TEST(blueprint, a_pin_whose_name_has_a_space_goes_long_hand) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 post = Add(blueprint, "net.post");
    Lit(blueprint, post, "Content Type", std::string("text/plain"));
    fixture.document.SetBlueprint(fixture.component, blueprint);
    const std::string xml = doc::Serializer::ToXml(fixture.document, true, &ui::StandardLibrary());
    CHECK(xml.find("<pin name=\"Content Type\" value=\"text/plain\"/>") != std::string::npos);

    doc::Document read;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(xml, read, &error, &ui::StandardLibrary()), error);
    for (const auto& [id, back] : read.Blueprints()) {
        const doc::BlueprintNode& node = back.graph.nodes.front();
        CHECK_EQ(std::get<std::string>(node.literals.at("Content Type")),
                 std::string("text/plain"));
    }
}

TEST(blueprint, markup_naming_a_node_that_does_not_exist_is_refused) {
    const std::string xml =
        "<vae version=\"4\">\n"
        "  <screen name=\"S\">\n"
        "    <blueprint><node id=\"1\" type=\"flow.teleport\" at=\"0 0\"/></blueprint>\n"
        "  </screen>\n"
        "</vae>\n";
    doc::Document read;
    std::string error;
    CHECK(!doc::Serializer::FromXml(xml, read, &error));
    CHECK(error.find("flow.teleport") != std::string::npos);
}

TEST(blueprint, markup_naming_a_pin_that_does_not_exist_is_refused) {
    const std::string xml =
        "<vae version=\"4\">\n"
        "  <screen name=\"S\">\n"
        "    <blueprint><node id=\"1\" type=\"app.toast\" at=\"0 0\" Minutes=\"2\"/></blueprint>\n"
        "  </screen>\n"
        "</vae>\n";
    doc::Document read;
    std::string error;
    CHECK(!doc::Serializer::FromXml(xml, read, &error));
    CHECK(error.find("Minutes") != std::string::npos);
}

TEST(blueprint, a_stock_widget_with_a_graph_is_a_fork) {
    doc::Document document;
    ui::StandardLibrary().Install("vae.std", 1, document);
    const std::size_t before = ui::StandardLibrary().Stock(document).size();
    CHECK(before > 0);

    // Give one of them logic and it is no longer something the binary can rebuild.
    Uuid first = Uuid::Invalid();
    for (Uuid root : document.Roots())
        if (const doc::Node* node = document.Find(root); node && node->IsComponent()) {
            first = root;
            break;
        }
    CHECK(first.Valid());
    doc::Blueprint blueprint;
    Add(blueprint, "event.mount");
    document.SetBlueprint(first, blueprint);
    CHECK_EQ(ui::StandardLibrary().Stock(document).size(), before - 1);
}

// ------------------------------------------------------------------------------ the compiler

TEST(blueprint, a_wire_between_two_types_that_do_not_convert_is_an_error) {
    doc::Blueprint blueprint;
    const u32 number = Add(blueprint, "make.number");
    const u32 setColour = Add(blueprint, "ui.setColour");
    Wire(blueprint, number, "Value", setColour, "Value");

    script::BlueprintProgram program;
    CHECK(!program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "cannot be wired into"));
}

TEST(blueprint, a_variable_that_is_not_there_is_an_error) {
    doc::Blueprint blueprint;
    Add(blueprint, "var.get", "missing");
    script::BlueprintProgram program;
    CHECK(!program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "'missing'"));
}

TEST(blueprint, two_events_for_the_same_thing_is_an_error) {
    doc::Blueprint blueprint;
    const u32 a = Add(blueprint, "event.clicked");
    const u32 b = Add(blueprint, "event.clicked");
    Lit(blueprint, a, "Node", std::string("Increment"));
    Lit(blueprint, b, "Node", std::string("Increment"));
    script::BlueprintProgram program;
    CHECK(!program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "already a"));

    // Two clicks on different widgets are two events, and perfectly ordinary.
    Lit(blueprint, b, "Node", std::string("Decrement"));
    CHECK(program.Compile(blueprint, "Counter"));
}

TEST(blueprint, a_value_worked_out_from_itself_is_an_error) {
    doc::Blueprint blueprint;
    const u32 a = Add(blueprint, "math.add");
    const u32 b = Add(blueprint, "math.add");
    Wire(blueprint, a, "Value", b, "A");
    script::BlueprintProgram compiles;
    CHECK(compiles.Compile(blueprint, "Counter"));

    // Close the loop, and the same blueprint is one that would evaluate for ever.
    Wire(blueprint, b, "Value", a, "A");
    script::BlueprintProgram program;
    CHECK(!program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "worked out from itself"));
}

TEST(blueprint, nothing_may_be_wired_to_a_pin_chosen_when_the_graph_is_drawn) {
    doc::Blueprint blueprint;
    const u32 text = Add(blueprint, "make.text");
    const u32 show = Add(blueprint, "ui.setText");
    Wire(blueprint, text, "Value", show, "Node");
    script::BlueprintProgram program;
    CHECK(!program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "chosen when the blueprint is drawn"));
}

TEST(blueprint, a_node_nothing_reaches_is_a_warning_and_still_runs_the_rest) {
    doc::Blueprint blueprint = Counter();
    Add(blueprint, "app.toast");                    // dropped on the canvas and never wired up
    script::BlueprintProgram program;
    CHECK(program.Compile(blueprint, "Counter"));   // still compiles
    CHECK(Says(program.Diagnostics(), "never runs", false));
}

TEST(blueprint, an_event_bound_to_nothing_is_a_warning) {
    doc::Blueprint blueprint;
    Add(blueprint, "event.clicked");                // no widget chosen
    script::BlueprintProgram program;
    CHECK(program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "does not say which", false));
}

// ------------------------------------------------------------------------------ running one

TEST(blueprint, a_counter_drawn_as_a_graph_counts) {
    Blueprinted fixture;
    fixture.document.SetBlueprint(fixture.component, Counter());
    fixture.Start();

    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.runtime.LiveCount(), std::size_t{ 1 });

    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("1"));
    fixture.Click("Increment");
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("3"));
    CHECK_NEAR(fixture.State("count"), 3.0);
}

TEST(blueprint, a_graph_that_does_not_compile_does_not_run_at_all) {
    Blueprinted fixture;
    doc::Blueprint blueprint = Counter();
    // One bad wire, and the rest of the blueprint must not half-run: half of it having run leaves the
    // screen in a state nobody drew.
    const u32 number = Add(blueprint, "make.number");
    const u32 colour = Add(blueprint, "ui.setColour");
    Wire(blueprint, number, "Value", colour, "Value");
    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();

    CHECK(fixture.blueprints->ErrorCount() > 0);
    CHECK_EQ(fixture.runtime.LiveCount(), std::size_t{ 0 });
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("-"));
}

TEST(blueprint, a_branch_runs_one_side) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "count", doc::PinType::Number, 0.0f });

    const u32 clicked = Add(blueprint, "event.clicked");
    Lit(blueprint, clicked, "Node", std::string("Increment"));
    const u32 get = Add(blueprint, "var.get", "count");
    const u32 less = Add(blueprint, "compare.less");
    Lit(blueprint, less, "B", 2.0f);
    const u32 branch = Add(blueprint, "flow.branch");
    const u32 yes = Add(blueprint, "ui.setText");
    Lit(blueprint, yes, "Node", std::string("Label"));
    Lit(blueprint, yes, "Value", std::string("low"));
    const u32 no = Add(blueprint, "ui.setText");
    Lit(blueprint, no, "Node", std::string("Label"));
    Lit(blueprint, no, "Value", std::string("high"));

    const u32 add = Add(blueprint, "math.add");
    Lit(blueprint, add, "B", 1.0f);
    const u32 set = Add(blueprint, "var.set", "count");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, add, "Value", set, "Value");
    Wire(blueprint, clicked, "Out", set, "In");
    Wire(blueprint, set, "Out", branch, "In");
    Wire(blueprint, get, "Value", less, "A");
    Wire(blueprint, less, "Value", branch, "Condition");
    Wire(blueprint, branch, "True", yes, "In");
    Wire(blueprint, branch, "False", no, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();

    fixture.Click("Increment");                 // count is now 1, so `count < 2` still holds
    CHECK_EQ(fixture.TextOf("Label"), std::string("low"));
    fixture.Click("Increment");
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("high"));
}

TEST(blueprint, a_for_loop_runs_its_body_once_per_number) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "total", doc::PinType::Number, 0.0f });

    const u32 mount = Add(blueprint, "event.mount");
    const u32 loop = Add(blueprint, "flow.forLoop");
    Lit(blueprint, loop, "First", 1.0f);
    Lit(blueprint, loop, "Last", 4.0f);
    const u32 get = Add(blueprint, "var.get", "total");
    const u32 add = Add(blueprint, "math.add");
    const u32 set = Add(blueprint, "var.set", "total");
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));

    Wire(blueprint, mount, "Out", loop, "In");
    Wire(blueprint, loop, "Body", set, "In");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, loop, "Index", add, "B");       // 1 + 2 + 3 + 4
    Wire(blueprint, add, "Value", set, "Value");
    Wire(blueprint, loop, "Done", show, "In");
    Wire(blueprint, get, "Value", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_NEAR(fixture.State("total"), 10.0);
    CHECK_EQ(fixture.TextOf("Label"), std::string("10"));
}

TEST(blueprint, a_while_loop_runs_until_its_condition_stops_holding) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "n", doc::PinType::Number, 0.0f });

    const u32 mount = Add(blueprint, "event.mount");
    const u32 loop = Add(blueprint, "flow.while");
    const u32 get = Add(blueprint, "var.get", "n");
    const u32 less = Add(blueprint, "compare.less");
    Lit(blueprint, less, "B", 5.0f);
    const u32 add = Add(blueprint, "math.add");
    Lit(blueprint, add, "B", 1.0f);
    const u32 set = Add(blueprint, "var.set", "n");

    Wire(blueprint, mount, "Out", loop, "In");
    Wire(blueprint, get, "Value", less, "A");
    Wire(blueprint, less, "Value", loop, "Condition");
    Wire(blueprint, loop, "Body", set, "In");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, add, "Value", set, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_NEAR(fixture.State("n"), 5.0);
}

TEST(blueprint, a_loop_that_never_ends_is_stopped_rather_than_hanging) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 loop = Add(blueprint, "flow.while");
    const u32 always = Add(blueprint, "make.bool");
    Lit(blueprint, always, "Value", true);
    const u32 body = Add(blueprint, "app.log");
    Lit(blueprint, body, "Level", std::string("trace"));

    Wire(blueprint, mount, "Out", loop, "In");
    Wire(blueprint, always, "Value", loop, "Condition");
    Wire(blueprint, loop, "Body", body, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    // The point of this test is that it RETURNS. Without the step budget it never would, which is
    // how the guard was checked: removing it hangs this case rather than failing it.
    fixture.Start();
    CHECK(fixture.runtime.LiveCount() == 1);
}

TEST(blueprint, do_once_passes_the_first_and_swallows_the_rest) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "hits", doc::PinType::Number, 0.0f });

    const u32 clicked = Add(blueprint, "event.clicked");
    Lit(blueprint, clicked, "Node", std::string("Increment"));
    const u32 once = Add(blueprint, "flow.doOnce");
    const u32 get = Add(blueprint, "var.get", "hits");
    const u32 add = Add(blueprint, "math.add");
    Lit(blueprint, add, "B", 1.0f);
    const u32 set = Add(blueprint, "var.set", "hits");

    Wire(blueprint, clicked, "Out", once, "In");
    Wire(blueprint, once, "Out", set, "In");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, add, "Value", set, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    fixture.Click("Increment");
    fixture.Click("Increment");
    fixture.Click("Increment");
    CHECK_NEAR(fixture.State("hits"), 1.0);
}

TEST(blueprint, a_delay_carries_on_when_the_timer_comes_due) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 before = Add(blueprint, "ui.setText");
    Lit(blueprint, before, "Node", std::string("Label"));
    Lit(blueprint, before, "Value", std::string("waiting"));
    const u32 delay = Add(blueprint, "flow.delay");
    Lit(blueprint, delay, "Seconds", 0.1f);
    const u32 after = Add(blueprint, "ui.setText");
    Lit(blueprint, after, "Node", std::string("Label"));
    Lit(blueprint, after, "Value", std::string("done"));

    Wire(blueprint, mount, "Out", before, "In");
    Wire(blueprint, before, "Out", delay, "In");
    Wire(blueprint, delay, "Done", after, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.TextOf("Label"), std::string("waiting"));
    CHECK(fixture.runtime.HasPendingTimers());

    for (int i = 0; i < 20; ++i) fixture.Frame(0.02f);
    CHECK_EQ(fixture.TextOf("Label"), std::string("done"));
}

TEST(blueprint, a_value_read_twice_in_one_step_is_worked_out_once) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 random = Add(blueprint, "math.random");
    Lit(blueprint, random, "Max", 1000000.0f);
    const u32 join = Add(blueprint, "text.join");
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));

    // The same pure node feeding both halves. Without the memo the two reads are two rolls, and
    // the halves of the label would almost never match.
    Wire(blueprint, random, "Value", join, "A");
    Wire(blueprint, random, "Value", join, "B");
    Wire(blueprint, join, "Value", show, "Value");
    Wire(blueprint, mount, "Out", show, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    const std::string label = fixture.TextOf("Label");
    CHECK(!label.empty());
    CHECK_EQ(label.substr(0, label.size() / 2), label.substr(label.size() / 2));
}

TEST(blueprint, a_number_wired_into_text_reads_as_what_it_says) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 number = Add(blueprint, "make.number");
    Lit(blueprint, number, "Value", 42.0f);
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));
    Wire(blueprint, mount, "Out", show, "In");
    Wire(blueprint, number, "Value", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    // "42", not "42.000000" — a label says a number the way a person writes one.
    CHECK_EQ(fixture.TextOf("Label"), std::string("42"));
}

TEST(blueprint, a_reload_keeps_what_is_on_screen) {
    Blueprinted fixture;
    fixture.document.SetBlueprint(fixture.component, Counter());
    fixture.Start();
    fixture.Click("Increment");
    fixture.Click("Increment");
    CHECK_NEAR(fixture.State("count"), 2.0);

    // Edit the blueprint and swap it in underneath the running app. The count is the state bag's, not
    // the module's, so it survives — which is the whole reason a variable lives there.
    doc::Blueprint blueprint = Counter();
    Add(blueprint, "app.toast");
    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.blueprints->Adopt(fixture.document);
    std::string error;
    CHECK_MESSAGE(fixture.runtime.Reload(&error), error);
    fixture.Frame();
    CHECK_NEAR(fixture.State("count"), 2.0);
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("3"));
}

TEST(blueprint, a_screen_sets_a_knob_the_component_declares) {
    // The relation Unreal draws between a Level Blueprint and an Actor's: the screen holds
    // instances, and it talks to them through what they declare, never through their insides.
    Blueprinted fixture;

    // The component exposes a `label` knob, and the text inside it is bound to it.
    doc::ComponentProperty label;
    label.name = "label";
    label.type = doc::ValueType::Text;
    label.defaultValue = std::string("-");
    fixture.document.SetComponentProperty(fixture.component, label);
    for (const Uuid id : fixture.document.Subtree(fixture.component))
        if (const doc::Node* node = fixture.document.Find(id); node && node->name == "Label")
            fixture.document.SetProp(id, doc::Prop::Text, doc::Binding{ "label" });

    // The SCREEN's blueprint sets it on the instance called Counter.
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 set = Add(blueprint, "ui.setProperty");
    Lit(blueprint, set, "Node", std::string("Counter"));
    Lit(blueprint, set, "Property", std::string("label"));
    Lit(blueprint, set, "Value", std::string("from the screen"));
    Wire(blueprint, mount, "Out", set, "In");

    // The instance has to be findable by name from the screen's point of view.
    fixture.document.Find(fixture.instance)->name = "Counter";
    fixture.document.Touch(fixture.instance);
    fixture.document.SetBlueprint(fixture.screen, blueprint);
    fixture.Start();
    fixture.Frame();
    fixture.Frame();

    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.TextOf("Label"), std::string("from the screen"));
}

TEST(blueprint, the_debugger_sees_which_wires_carried_a_step) {
    Blueprinted fixture;
    fixture.document.SetBlueprint(fixture.component, Counter());
    fixture.Start();
    fixture.blueprints->SetWatching(true);
    fixture.blueprints->TakeFlow();

    fixture.Click("Increment");
    const std::vector<script::BlueprintHost::Flow> flow = fixture.blueprints->TakeFlow();
    CHECK(!flow.empty());
    CHECK_EQ(flow.front().component, std::string("Counter"));
    // And the value that crossed a data wire is there to put on the pin.
    bool sawOne = false;
    for (const auto& [key, watched] : fixture.blueprints->Values())
        if (watched.text == "1") sawOne = true;
    CHECK(sawOne);
}

// ------------------------------------------------------------------------------ functions

namespace {

    // A function with a body, added to a blueprint and handed back so a test can draw in it.
    doc::BlueprintFunction& Function(doc::Blueprint& blueprint, std::string name,
                                     std::vector<doc::BlueprintParam> params = {},
                                     std::vector<doc::BlueprintParam> returns = {},
                                     bool pure = false, bool event = false) {
        doc::BlueprintFunction function;
        function.name = std::move(name);
        function.params = std::move(params);
        function.returns = std::move(returns);
        function.pure = pure;
        function.event = event;
        blueprint.SetFunction(std::move(function));
        return *blueprint.FindFunction(blueprint.functions.back().name);
    }

}

TEST(blueprint, a_function_takes_values_and_hands_one_back) {
    Blueprinted fixture;
    doc::Blueprint blueprint;

    // double(n) = n * 2
    doc::BlueprintFunction& twice = Function(blueprint, "Twice",
        { { "n", doc::PinType::Number, 0.0f } },
        { { "out", doc::PinType::Number, 0.0f } });
    const u32 entry = Add(blueprint, "func.entry", {}, &twice.body);
    const u32 times = Add(blueprint, "math.multiply", {}, &twice.body);
    Lit(blueprint, times, "B", 2.0f, &twice.body);
    const u32 ret = Add(blueprint, "func.return", {}, &twice.body);
    Wire(blueprint, entry, "Out", ret, "In", &twice.body);
    Wire(blueprint, entry, "n", times, "A", &twice.body);
    Wire(blueprint, times, "Value", ret, "out", &twice.body);

    // On Mount: show Twice(21).
    const u32 mount = Add(blueprint, "event.mount");
    const u32 call = Add(blueprint, "func.call", "Twice");
    Lit(blueprint, call, "n", 21.0f);
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));
    Wire(blueprint, mount, "Out", call, "In");
    Wire(blueprint, call, "Out", show, "In");
    Wire(blueprint, call, "out", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.TextOf("Label"), std::string("42"));
}

TEST(blueprint, a_pure_function_is_worked_out_where_it_is_read) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    doc::BlueprintFunction& shout = Function(blueprint, "Shout",
        { { "word", doc::PinType::Text, std::string() } },
        { { "out", doc::PinType::Text, std::string() } }, true);
    const u32 entry = Add(blueprint, "func.entry", {}, &shout.body);
    const u32 upper = Add(blueprint, "text.upper", {}, &shout.body);
    const u32 ret = Add(blueprint, "func.return", {}, &shout.body);
    Wire(blueprint, entry, "word", upper, "Text", &shout.body);
    Wire(blueprint, upper, "Value", ret, "out", &shout.body);

    const u32 mount = Add(blueprint, "event.mount");
    const u32 call = Add(blueprint, "func.call", "Shout");
    Lit(blueprint, call, "word", std::string("hello"));
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));
    // No execution wire into the call at all: it is pure, so reading it is what runs it.
    Wire(blueprint, mount, "Out", show, "In");
    Wire(blueprint, call, "out", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.TextOf("Label"), std::string("HELLO"));
}

TEST(blueprint, a_local_starts_fresh_every_call) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "total", doc::PinType::Number, 0.0f });

    doc::BlueprintFunction& bump = Function(blueprint, "Bump");
    bump.locals.push_back({ "seen", doc::PinType::Number, 0.0f });
    const u32 entry = Add(blueprint, "func.entry", {}, &bump.body);
    // seen = seen + 1, then total = total + seen. If the local survived the call, the second
    // call would add two rather than one.
    const u32 getSeen = Add(blueprint, "var.get", "seen", &bump.body);
    const u32 addSeen = Add(blueprint, "math.add", {}, &bump.body);
    Lit(blueprint, addSeen, "B", 1.0f, &bump.body);
    const u32 setSeen = Add(blueprint, "var.set", "seen", &bump.body);
    const u32 getTotal = Add(blueprint, "var.get", "total", &bump.body);
    const u32 addTotal = Add(blueprint, "math.add", {}, &bump.body);
    const u32 setTotal = Add(blueprint, "var.set", "total", &bump.body);
    Wire(blueprint, entry, "Out", setSeen, "In", &bump.body);
    Wire(blueprint, getSeen, "Value", addSeen, "A", &bump.body);
    Wire(blueprint, addSeen, "Value", setSeen, "Value", &bump.body);
    Wire(blueprint, setSeen, "Out", setTotal, "In", &bump.body);
    Wire(blueprint, getTotal, "Value", addTotal, "A", &bump.body);
    Wire(blueprint, setSeen, "Value", addTotal, "B", &bump.body);
    Wire(blueprint, addTotal, "Value", setTotal, "Value", &bump.body);

    const u32 mount = Add(blueprint, "event.mount");
    const u32 a = Add(blueprint, "func.call", "Bump");
    const u32 b = Add(blueprint, "func.call", "Bump");
    Wire(blueprint, mount, "Out", a, "In");
    Wire(blueprint, a, "Out", b, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    // 1 + 1, not 1 + 2.
    CHECK_NEAR(fixture.State("total"), 2.0);
}

TEST(blueprint, a_function_may_not_wait) {
    doc::Blueprint blueprint;
    doc::BlueprintFunction& slow = Function(blueprint, "Slow");
    const u32 entry = Add(blueprint, "func.entry", {}, &slow.body);
    const u32 delay = Add(blueprint, "flow.delay", {}, &slow.body);
    Wire(blueprint, entry, "Out", delay, "In", &slow.body);

    script::BlueprintProgram program;
    CHECK(!program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "cannot be inside a function"));

    // A custom event is exactly the thing that may.
    blueprint.RemoveFunction("Slow");
    doc::BlueprintFunction& later = Function(blueprint, "Later", {}, {}, false, true);
    const u32 e2 = Add(blueprint, "func.entry", {}, &later.body);
    const u32 d2 = Add(blueprint, "flow.delay", {}, &later.body);
    Wire(blueprint, e2, "Out", d2, "In", &later.body);
    CHECK(program.Compile(blueprint, "Counter"));
}

TEST(blueprint, a_custom_event_carries_on_after_a_delay) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    doc::BlueprintFunction& later = Function(blueprint, "Later",
        { { "word", doc::PinType::Text, std::string() } }, {}, false, true);
    const u32 entry = Add(blueprint, "func.entry", {}, &later.body);
    const u32 delay = Add(blueprint, "flow.delay", {}, &later.body);
    Lit(blueprint, delay, "Seconds", 0.1f, &later.body);
    const u32 show = Add(blueprint, "ui.setText", {}, &later.body);
    Lit(blueprint, show, "Node", std::string("Label"), &later.body);
    Wire(blueprint, entry, "Out", delay, "In", &later.body);
    Wire(blueprint, delay, "Done", show, "In", &later.body);
    // The parameter is read on the far side of the wait, which is the case that needs the
    // arguments kept while the timer runs.
    Wire(blueprint, entry, "word", show, "Value", &later.body);

    const u32 mount = Add(blueprint, "event.mount");
    const u32 call = Add(blueprint, "func.call", "Later");
    Lit(blueprint, call, "word", std::string("after"));
    Wire(blueprint, mount, "Out", call, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.TextOf("Label"), std::string("-"));
    for (int i = 0; i < 20; ++i) fixture.Frame(0.02f);
    CHECK_EQ(fixture.TextOf("Label"), std::string("after"));
}

TEST(blueprint, a_function_that_calls_itself_is_bounded) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    doc::BlueprintFunction& forever = Function(blueprint, "Forever");
    const u32 entry = Add(blueprint, "func.entry", {}, &forever.body);
    const u32 again = Add(blueprint, "func.call", "Forever", &forever.body);
    Wire(blueprint, entry, "Out", again, "In", &forever.body);

    const u32 mount = Add(blueprint, "event.mount");
    const u32 call = Add(blueprint, "func.call", "Forever");
    Wire(blueprint, mount, "Out", call, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    // The point of this test is that it RETURNS. Recursion is allowed — Unreal allows it too —
    // and the depth guard is what stops it being a stack overflow with no message.
    fixture.Start();
    CHECK(fixture.runtime.LiveCount() == 1);
}

TEST(blueprint, a_function_survives_a_round_trip_through_markup) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "names", doc::PinType::List, std::string("Ada\nGrace") });
    doc::BlueprintFunction& show = Function(blueprint, "Show",
        { { "n", doc::PinType::Number, 3.0f } },
        { { "text", doc::PinType::Text, std::string() } });
    show.locals.push_back({ "tmp", doc::PinType::Text, std::string("x") });
    const u32 entry = Add(blueprint, "func.entry", {}, &show.body);
    const u32 ret = Add(blueprint, "func.return", {}, &show.body);
    Wire(blueprint, entry, "Out", ret, "In", &show.body);
    Function(blueprint, "Later", {}, {}, false, true);
    Add(blueprint, "func.entry", {}, &blueprint.FindFunction("Later")->body);

    fixture.document.SetBlueprint(fixture.component, blueprint);
    const std::string first = doc::Serializer::ToXml(fixture.document, true,
                                                     &ui::StandardLibrary());
    CHECK(first.find("<function name=\"Show\">") != std::string::npos);
    CHECK(first.find("<event name=\"Later\">") != std::string::npos);
    CHECK(first.find("<local name=\"tmp\"") != std::string::npos);
    CHECK(first.find("type=\"list\"") != std::string::npos);

    doc::Document read;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(first, read, &error, &ui::StandardLibrary()), error);
    const std::string second = doc::Serializer::ToXml(read, true, &ui::StandardLibrary());
    CHECK_MESSAGE(first == second, "re-encoding a blueprint did not produce the same markup");

    for (const auto& [id, back] : read.Blueprints()) {
        CHECK_EQ(back.functions.size(), std::size_t{ 2 });
        const doc::BlueprintFunction* readShow = back.FindFunction("Show");
        CHECK(readShow != nullptr);
        if (readShow) {
            CHECK_EQ(readShow->params.size(), std::size_t{ 1 });
            CHECK(readShow->params.front().type == doc::PinType::Number);
            CHECK_EQ(readShow->locals.size(), std::size_t{ 1 });
            CHECK_EQ(readShow->body.nodes.size(), std::size_t{ 2 });
        }
        CHECK(back.FindFunction("Later") != nullptr);
        CHECK(back.FindFunction("Later")->event);
        CHECK(back.FindVariable("names") != nullptr);
        CHECK(back.FindVariable("names")->type == doc::PinType::List);
    }
}

// ------------------------------------------------------------------------------ collections

TEST(blueprint, a_list_is_built_walked_and_asked_about) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "names", doc::PinType::List, std::string() });
    blueprint.SetVariable({ "seen", doc::PinType::Text, std::string() });

    const u32 mount = Add(blueprint, "event.mount");
    // names = split("Ada,Grace,Alan", ",")
    const u32 split = Add(blueprint, "list.split");
    Lit(blueprint, split, "Text", std::string("Ada,Grace,Alan"));
    const u32 set = Add(blueprint, "var.set", "names");
    Wire(blueprint, mount, "Out", set, "In");
    Wire(blueprint, split, "Value", set, "Value");

    // for each name: seen = seen + name
    const u32 each = Add(blueprint, "flow.forEach");
    const u32 names = Add(blueprint, "var.get", "names");
    Wire(blueprint, set, "Out", each, "In");
    Wire(blueprint, names, "Value", each, "List");
    const u32 getSeen = Add(blueprint, "var.get", "seen");
    const u32 join = Add(blueprint, "text.join");
    const u32 setSeen = Add(blueprint, "var.set", "seen");
    Wire(blueprint, each, "Body", setSeen, "In");
    Wire(blueprint, getSeen, "Value", join, "A");
    Wire(blueprint, each, "Element", join, "B");
    Wire(blueprint, join, "Value", setSeen, "Value");

    // Done: show how many there were.
    const u32 length = Add(blueprint, "list.length");
    Wire(blueprint, names, "Value", length, "List");
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));
    Wire(blueprint, each, "Done", show, "In");
    Wire(blueprint, length, "Length", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.TextOf("Label"), std::string("3"));
    CHECK_EQ(fixture.StateText("seen"), std::string("AdaGraceAlan"));
    // A list variable is text in the bag, which is what makes it survive a reload.
    CHECK_EQ(fixture.StateText("names"), std::string("Ada\nGrace\nAlan"));
}

TEST(blueprint, a_break_leaves_the_loop_it_is_in) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "count", doc::PinType::Number, 0.0f });

    const u32 mount = Add(blueprint, "event.mount");
    const u32 loop = Add(blueprint, "flow.forLoop");
    Lit(blueprint, loop, "First", 1.0f);
    Lit(blueprint, loop, "Last", 100.0f);
    const u32 get = Add(blueprint, "var.get", "count");
    const u32 add = Add(blueprint, "math.add");
    Lit(blueprint, add, "B", 1.0f);
    const u32 set = Add(blueprint, "var.set", "count");
    const u32 past = Add(blueprint, "compare.greaterEqual");
    Lit(blueprint, past, "B", 3.0f);
    const u32 branch = Add(blueprint, "flow.branch");
    const u32 stop = Add(blueprint, "flow.break");

    Wire(blueprint, mount, "Out", loop, "In");
    Wire(blueprint, loop, "Body", set, "In");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, add, "Value", set, "Value");
    Wire(blueprint, set, "Out", branch, "In");
    Wire(blueprint, set, "Value", past, "A");
    Wire(blueprint, past, "Value", branch, "Condition");
    Wire(blueprint, branch, "True", stop, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_NEAR(fixture.State("count"), 3.0);
}

TEST(blueprint, a_break_with_no_loop_around_it_is_an_error) {
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 stop = Add(blueprint, "flow.break");
    Wire(blueprint, mount, "Out", stop, "In");
    script::BlueprintProgram program;
    CHECK(!program.Compile(blueprint, "Counter"));
    CHECK(Says(program.Diagnostics(), "no loop around this"));
}

TEST(blueprint, a_map_holds_things_by_name) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "who", doc::PinType::Map, std::string() });

    const u32 mount = Add(blueprint, "event.mount");
    const u32 get = Add(blueprint, "var.get", "who");
    const u32 put = Add(blueprint, "map.set");
    Lit(blueprint, put, "Key", std::string("name"));
    Lit(blueprint, put, "Value", std::string("Ada"));
    const u32 set = Add(blueprint, "var.set", "who");
    Wire(blueprint, mount, "Out", put, "In");
    Wire(blueprint, get, "Value", put, "Map");
    Wire(blueprint, put, "Out", set, "In");
    Wire(blueprint, put, "Map", set, "Value");

    const u32 read = Add(blueprint, "map.get");
    Lit(blueprint, read, "Key", std::string("name"));
    Lit(blueprint, read, "Default", std::string("nobody"));
    Wire(blueprint, put, "Map", read, "Map");
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));
    Wire(blueprint, set, "Out", show, "In");
    Wire(blueprint, read, "Value", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.TextOf("Label"), std::string("Ada"));
    CHECK_EQ(fixture.StateText("who"), std::string("name\tAda"));
}

TEST(blueprint, a_collection_hands_back_a_new_one_rather_than_changing_the_old) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "a", doc::PinType::List, std::string("one") });

    const u32 mount = Add(blueprint, "event.mount");
    const u32 get = Add(blueprint, "var.get", "a");
    const u32 add = Add(blueprint, "list.add");
    Lit(blueprint, add, "Value", std::string("two"));
    Wire(blueprint, mount, "Out", add, "In");
    Wire(blueprint, get, "Value", add, "List");
    // The result is deliberately NOT wired back into the variable.
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));
    const u32 join = Add(blueprint, "list.join");
    Lit(blueprint, join, "Separator", std::string("+"));
    Wire(blueprint, get, "Value", join, "List");
    Wire(blueprint, add, "Out", show, "In");
    Wire(blueprint, join, "Value", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    // The variable still says one thing: Add handed back a longer list and nobody kept it.
    CHECK_EQ(fixture.TextOf("Label"), std::string("one"));
    CHECK_EQ(fixture.StateText("a"), std::string("one"));
}

// ------------------------------------------------------------------------------ the rest of flow

TEST(blueprint, a_switch_runs_the_case_that_matches) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "count", doc::PinType::Number, 0.0f });

    const u32 clicked = Add(blueprint, "event.clicked");
    Lit(blueprint, clicked, "Node", std::string("Increment"));
    const u32 get = Add(blueprint, "var.get", "count");
    const u32 add = Add(blueprint, "math.add");
    Lit(blueprint, add, "B", 1.0f);
    const u32 set = Add(blueprint, "var.set", "count");
    Wire(blueprint, clicked, "Out", set, "In");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, add, "Value", set, "Value");

    const u32 pick = Add(blueprint, "switch.number");
    Lit(blueprint, pick, "Case 0", 1.0f);
    Lit(blueprint, pick, "Case 1", 2.0f);
    Wire(blueprint, set, "Out", pick, "In");
    Wire(blueprint, set, "Value", pick, "Value");

    const auto Say = [&](const char* what) {
        const u32 node = Add(blueprint, "ui.setText");
        Lit(blueprint, node, "Node", std::string("Label"));
        Lit(blueprint, node, "Value", std::string(what));
        return node;
    };
    Wire(blueprint, pick, "Case 0", Say("one"), "In");
    Wire(blueprint, pick, "Case 1", Say("two"), "In");
    Wire(blueprint, pick, "Default", Say("many"), "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("one"));
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("two"));
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("many"));
}

TEST(blueprint, do_n_and_flip_flop_and_the_gate) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "hits", doc::PinType::Number, 0.0f });

    const u32 clicked = Add(blueprint, "event.clicked");
    Lit(blueprint, clicked, "Node", std::string("Increment"));
    const u32 twice = Add(blueprint, "flow.doN");
    Lit(blueprint, twice, "N", 2.0f);
    const u32 get = Add(blueprint, "var.get", "hits");
    const u32 add = Add(blueprint, "math.add");
    Lit(blueprint, add, "B", 1.0f);
    const u32 set = Add(blueprint, "var.set", "hits");
    Wire(blueprint, clicked, "Out", twice, "In");
    Wire(blueprint, twice, "Out", set, "In");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, add, "Value", set, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    fixture.Click("Increment");
    fixture.Click("Increment");
    fixture.Click("Increment");
    fixture.Click("Increment");
    CHECK_NEAR(fixture.State("hits"), 2.0);
}

TEST(blueprint, a_flip_flop_alternates) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 clicked = Add(blueprint, "event.clicked");
    Lit(blueprint, clicked, "Node", std::string("Increment"));
    const u32 flip = Add(blueprint, "flow.flipFlop");
    Wire(blueprint, clicked, "Out", flip, "In");
    const auto Say = [&](const char* what) {
        const u32 node = Add(blueprint, "ui.setText");
        Lit(blueprint, node, "Node", std::string("Label"));
        Lit(blueprint, node, "Value", std::string(what));
        return node;
    };
    Wire(blueprint, flip, "A", Say("first"), "In");
    Wire(blueprint, flip, "B", Say("second"), "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("first"));
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("second"));
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("first"));
}

TEST(blueprint, the_maths_and_the_text_do_what_they_say) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 show = Add(blueprint, "ui.setText");
    Lit(blueprint, show, "Node", std::string("Label"));
    Wire(blueprint, mount, "Out", show, "In");

    // 09 — the shape of a clock, which needs Pad, and a whole chain of pure nodes to make it.
    const u32 root = Add(blueprint, "math.sqrt");
    Lit(blueprint, root, "Value", 81.0f);
    const u32 asText = Add(blueprint, "text.fromNumber");
    Wire(blueprint, root, "Value", asText, "Number");
    const u32 pad = Add(blueprint, "text.pad");
    Lit(blueprint, pad, "Width", 2.0f);
    Lit(blueprint, pad, "With", std::string("0"));
    Wire(blueprint, asText, "Text", pad, "Text");
    Wire(blueprint, pad, "Value", show, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    CHECK_EQ(fixture.TextOf("Label"), std::string("09"));
}

// ------------------------------------------------------------------------------ the debugger

TEST(blueprint, a_breakpoint_stops_the_run_and_continue_finishes_it) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    const u32 mount = Add(blueprint, "event.mount");
    const u32 first = Add(blueprint, "ui.setText");
    Lit(blueprint, first, "Node", std::string("Label"));
    Lit(blueprint, first, "Value", std::string("one"));
    const u32 second = Add(blueprint, "ui.setText");
    Lit(blueprint, second, "Node", std::string("Label"));
    Lit(blueprint, second, "Value", std::string("two"));
    Wire(blueprint, mount, "Out", first, "In");
    Wire(blueprint, first, "Out", second, "In");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    // Start the host, put a breakpoint on the second node, and only then let it mount.
    auto host = CreateScope<script::BlueprintHost>();
    fixture.blueprints = host.get();
    fixture.runtime.AddHost(std::move(host));
    fixture.blueprints->Adopt(fixture.document);
    fixture.blueprints->SetBreakpoint("Counter", second, true);
    fixture.Frame();

    CHECK(fixture.blueprints->Suspended());
    CHECK_EQ(fixture.blueprints->Stopped().node, second);
    // The first statement ran; the second is where it stopped, so it has not.
    CHECK_EQ(fixture.TextOf("Label"), std::string("one"));

    fixture.blueprints->Continue();
    fixture.Frame();
    CHECK(!fixture.blueprints->Suspended());
    CHECK_EQ(fixture.TextOf("Label"), std::string("two"));
}

TEST(blueprint, a_breakpoint_inside_a_loop_picks_up_where_it_stopped) {
    Blueprinted fixture;
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "count", doc::PinType::Number, 0.0f });
    const u32 mount = Add(blueprint, "event.mount");
    const u32 loop = Add(blueprint, "flow.forLoop");
    Lit(blueprint, loop, "First", 1.0f);
    Lit(blueprint, loop, "Last", 5.0f);
    const u32 get = Add(blueprint, "var.get", "count");
    const u32 add = Add(blueprint, "math.add");
    Lit(blueprint, add, "B", 1.0f);
    const u32 set = Add(blueprint, "var.set", "count");
    Wire(blueprint, mount, "Out", loop, "In");
    Wire(blueprint, loop, "Body", set, "In");
    Wire(blueprint, get, "Value", add, "A");
    Wire(blueprint, add, "Value", set, "Value");

    fixture.document.SetBlueprint(fixture.component, blueprint);
    auto host = CreateScope<script::BlueprintHost>();
    fixture.blueprints = host.get();
    fixture.runtime.AddHost(std::move(host));
    fixture.blueprints->Adopt(fixture.document);
    fixture.blueprints->SetBreakpoint("Counter", set, true);
    fixture.Frame();

    // Stopped on the first turn, before the body has done anything.
    CHECK(fixture.blueprints->Suspended());
    CHECK_NEAR(fixture.State("count"), 0.0);

    // Every Continue runs one more turn and stops again on the next, because the breakpoint is
    // inside the loop. Five turns, then it is done — which is the resume carrying the loop's
    // place across the pause.
    for (int i = 0; i < 5; ++i) {
        CHECK(fixture.blueprints->Suspended());
        fixture.blueprints->Continue();
    }
    CHECK(!fixture.blueprints->Suspended());
    CHECK_NEAR(fixture.State("count"), 5.0);
}

// ---------------------------------------------------------------------------- collapsing

// The ids Counter() mints, in the order it mints them, so a collapse test can name the nodes it
// is collapsing without rebuilding the blueprint by hand.
struct CounterIds { u32 clicked, get, add, set, show; };
CounterIds Ids(const doc::Blueprint& blueprint) {
    CounterIds ids{};
    for (const doc::BlueprintNode& node : blueprint.graph.nodes) {
        if (node.type == "event.clicked") ids.clicked = node.id;
        else if (node.type == "var.get")  ids.get = node.id;
        else if (node.type == "math.add") ids.add = node.id;
        else if (node.type == "var.set")  ids.set = node.id;
        else if (node.type == "ui.setText") ids.show = node.id;
    }
    return ids;
}

TEST(blueprint, a_collapsed_selection_still_counts) {
    doc::Blueprint blueprint = Counter();
    const CounterIds ids = Ids(blueprint);

    // Everything the click leads to. One execution wire crosses the boundary and no values do,
    // so the function takes nothing and hands nothing back.
    CHECK(doc::CollapseIntoFunction(blueprint, {}, { ids.get, ids.add, ids.set, ids.show },
                                    "Bump") == doc::CollapseResult::Ok);

    const doc::BlueprintFunction* made = blueprint.FindFunction("Bump");
    CHECK(made != nullptr);
    CHECK(!made->pure);
    CHECK(made->params.empty());
    CHECK(made->returns.empty());
    // The four that moved, plus the entry and the return put down with them.
    CHECK_EQ(made->body.nodes.size(), std::size_t{ 6 });

    // What is left is the event and one call.
    CHECK_EQ(blueprint.graph.nodes.size(), std::size_t{ 2 });
    const doc::BlueprintNode* call = blueprint.graph.FindType("func.call");
    CHECK(call != nullptr);
    CHECK_EQ(call->target, std::string("Bump"));
    CHECK_EQ(blueprint.graph.links.size(), std::size_t{ 1 });
    CHECK_EQ(blueprint.graph.links[0].from, ids.clicked);
    CHECK_EQ(blueprint.graph.links[0].to, call->id);

    // And it does what it did before it was moved, which is the only thing that settles it.
    Blueprinted fixture;
    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    fixture.Click("Increment");
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("2"));
}

TEST(blueprint, a_value_read_from_outside_becomes_a_parameter) {
    doc::Blueprint blueprint = Counter();
    const CounterIds ids = Ids(blueprint);

    // The Add stays outside, so the value it works out has to be handed in.
    CHECK(doc::CollapseIntoFunction(blueprint, {}, { ids.set, ids.show }, "Show")
          == doc::CollapseResult::Ok);

    const doc::BlueprintFunction* made = blueprint.FindFunction("Show");
    CHECK(made != nullptr);
    CHECK_EQ(made->params.size(), std::size_t{ 1 });
    CHECK_EQ(made->params[0].name, std::string("Value"));
    CHECK(made->params[0].type == doc::PinType::Number);
    CHECK(made->returns.empty());

    // One value wire into the call and one execution wire, and the Add is still outside feeding it.
    const doc::BlueprintNode* call = blueprint.graph.FindType("func.call");
    CHECK(call != nullptr);
    const auto* wire = std::ranges::find_if(blueprint.graph.links, [&](const auto& link) {
        return link.to == call->id && link.toPin == "Value";
    }).base();
    CHECK(wire != blueprint.graph.links.data() + blueprint.graph.links.size());
    CHECK_EQ(wire->from, ids.add);

    Blueprinted fixture;
    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    fixture.Click("Increment");
    fixture.Click("Increment");
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("3"));
    CHECK_NEAR(fixture.State("count"), 3.0);
}

TEST(blueprint, expressions_alone_collapse_to_a_pure_function) {
    doc::Blueprint blueprint = Counter();
    const CounterIds ids = Ids(blueprint);

    // Nothing but the Get and the Add: no execution wire touches either, so what they are is an
    // expression, and what they collapse to has to be one too.
    CHECK(doc::CollapseIntoFunction(blueprint, {}, { ids.get, ids.add }, "Next")
          == doc::CollapseResult::Ok);

    const doc::BlueprintFunction* made = blueprint.FindFunction("Next");
    CHECK(made != nullptr);
    CHECK(made->pure);
    CHECK(made->params.empty());
    CHECK_EQ(made->returns.size(), std::size_t{ 1 });
    CHECK_EQ(made->returns[0].name, std::string("Value"));

    Blueprinted fixture;
    fixture.document.SetBlueprint(fixture.component, blueprint);
    fixture.Start();
    CHECK_EQ(fixture.blueprints->ErrorCount(), std::size_t{ 0 });
    fixture.Click("Increment");
    CHECK_EQ(fixture.TextOf("Label"), std::string("1"));
}

TEST(blueprint, what_cannot_be_collapsed_says_so) {
    {   // Nothing selected.
        doc::Blueprint blueprint = Counter();
        CHECK(doc::CollapseIntoFunction(blueprint, {}, {}, "F") == doc::CollapseResult::Empty);
    }
    {   // An event is reached from outside the blueprint, so a function is not where it can live.
        doc::Blueprint blueprint = Counter();
        const CounterIds ids = Ids(blueprint);
        CHECK(doc::CollapseIntoFunction(blueprint, {}, { ids.clicked, ids.set }, "F")
              == doc::CollapseResult::HoldsEvent);
        CHECK(blueprint.functions.empty());
    }
    {   // A name that is already taken.
        doc::Blueprint blueprint = Counter();
        const CounterIds ids = Ids(blueprint);
        doc::BlueprintFunction taken;
        taken.name = "F";
        blueprint.SetFunction(std::move(taken));
        CHECK(doc::CollapseIntoFunction(blueprint, {}, { ids.set }, "F")
              == doc::CollapseResult::NameTaken);
    }
    {   // A Delay suspends the canvas it is on, and a function has to have finished by the time
        // the call hands back.
        doc::Blueprint blueprint = Counter();
        const CounterIds ids = Ids(blueprint);
        const u32 delay = Add(blueprint, "flow.delay");
        CHECK(doc::CollapseIntoFunction(blueprint, {}, { ids.set, delay }, "F")
              == doc::CollapseResult::HoldsLatent);
    }
    {   // Two execution wires in from outside, reaching two different nodes: a function has one
        // way in, and choosing which of the two it is would be the editor guessing.
        doc::Blueprint blueprint = Counter();
        const CounterIds ids = Ids(blueprint);
        const u32 second = Add(blueprint, "event.clicked");
        Lit(blueprint, second, "Node", std::string("Increment"));
        Wire(blueprint, second, "Out", ids.show, "In");
        CHECK(doc::CollapseIntoFunction(blueprint, {}, { ids.set, ids.show }, "F")
              == doc::CollapseResult::ManyEntries);
    }
    {   // And two ways out. A Branch inside with both sides landing outside is the smallest one
        // of these there is.
        doc::Blueprint blueprint = Counter();
        const CounterIds ids = Ids(blueprint);
        const u32 branch = Add(blueprint, "flow.branch");
        const u32 other = Add(blueprint, "ui.setText");
        Lit(blueprint, other, "Node", std::string("Label"));
        Wire(blueprint, branch, "True", ids.show, "In");
        Wire(blueprint, branch, "False", other, "In");
        CHECK(doc::CollapseIntoFunction(blueprint, {}, { branch }, "F")
              == doc::CollapseResult::ManyExits);
    }
}

TEST(blueprint, a_collapse_inside_a_function_finds_its_canvas_again) {
    // Adding the function may move the vector the canvas being collapsed lives in. This is the
    // test that fails, loudly, if the canvas is not found again after that.
    doc::Blueprint blueprint;
    blueprint.SetVariable({ "count", doc::PinType::Number, 0.0f });

    doc::BlueprintFunction outer;
    outer.name = "Outer";
    blueprint.SetFunction(std::move(outer));
    doc::BlueprintCanvas& body = blueprint.FindFunction("Outer")->body;

    const u32 entry = Add(blueprint, "func.entry", {}, &body);
    const u32 set = Add(blueprint, "var.set", "count", &body);
    Lit(blueprint, set, "Value", 7.0f, &body);
    const u32 ret = Add(blueprint, "func.return", {}, &body);
    Wire(blueprint, entry, "Out", set, "In", &body);
    Wire(blueprint, set, "Out", ret, "In", &body);

    CHECK(doc::CollapseIntoFunction(blueprint, "Outer", { set }, "Inner")
          == doc::CollapseResult::Ok);
    CHECK_EQ(blueprint.functions.size(), std::size_t{ 2 });

    const doc::BlueprintCanvas* outerBody = blueprint.CanvasFor("Outer");
    CHECK(outerBody != nullptr);
    // Entry, Return and the call that replaced the Set.
    CHECK_EQ(outerBody->nodes.size(), std::size_t{ 3 });
    CHECK(outerBody->FindType("func.call") != nullptr);

    script::BlueprintProgram program;
    CHECK(program.Compile(blueprint, "Counter"));
}
