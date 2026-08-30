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
    u32 Add(doc::Blueprint& blueprint, std::string type, std::string target = {}) {
        doc::BlueprintNode node;
        node.type = std::move(type);
        node.target = std::move(target);
        return blueprint.AddNode(std::move(node));
    }

    void Wire(doc::Blueprint& blueprint, u32 from, std::string fromPin, u32 to, std::string toPin) {
        blueprint.AddLink({ 0, from, std::move(fromPin), to, std::move(toPin) });
    }

    void Lit(doc::Blueprint& blueprint, u32 node, const std::string& pin, doc::Value value) {
        if (doc::BlueprintNode* found = blueprint.Find(node)) found->literals[pin] = std::move(value);
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
        blueprint.SetVariable({ "count", doc::ValueType::Number, 0.0f });

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
        const bool event = type.category == doc::BlueprintCategory::Event;
        bool hasExecIn = false;
        for (const doc::PinSpec& pin : type.inputs)
            if (pin.type == doc::PinType::Exec) hasExecIn = true;
        if (event || type.pure) CHECK_MESSAGE(!hasExecIn, std::string(type.id));
        else                    CHECK_MESSAGE(hasExecIn, std::string(type.id));
    }
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
    blueprint.DisplaceAt(c, "A", true, false);
    Wire(blueprint, b, "Value", c, "A");
    CHECK_EQ(blueprint.links.size(), std::size_t{ 1 });
    CHECK_EQ(blueprint.LinkInto(c, "A")->from, b);

    // A data OUTPUT fans out freely — the same value read in three places.
    Wire(blueprint, a, "Value", c, "B");
    blueprint.DisplaceAt(a, "Value", false, false);
    CHECK_EQ(blueprint.links.size(), std::size_t{ 2 });
}

// ------------------------------------------------------------------------------ the codec

TEST(blueprint, a_graph_survives_a_round_trip_through_markup) {
    Blueprinted fixture;
    doc::Blueprint blueprint = Counter();
    blueprint.comments.push_back({ 0, "counting", { 10.0f, 20.0f }, { 300.0f, 150.0f } });
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
        CHECK_EQ(back->nodes.size(), blueprint.nodes.size());
        CHECK_EQ(back->links.size(), blueprint.links.size());
        CHECK_EQ(back->variables.size(), std::size_t{ 1 });
        CHECK_EQ(back->comments.size(), std::size_t{ 1 });
        CHECK_EQ(back->comments.front().text, std::string("counting"));
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
        const doc::BlueprintNode& node = back.nodes.front();
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
    blueprint.SetVariable({ "count", doc::ValueType::Number, 0.0f });

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
    blueprint.SetVariable({ "total", doc::ValueType::Number, 0.0f });

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
    blueprint.SetVariable({ "n", doc::ValueType::Number, 0.0f });

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
    blueprint.SetVariable({ "hits", doc::ValueType::Number, 0.0f });

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
