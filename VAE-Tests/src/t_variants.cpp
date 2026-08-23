#include "Test.h"

#include "vae/doc/Document.h"
#include "vae/doc/Serializer.h"
#include "vae/ui/Library.h"

using namespace vae;
using namespace vae::doc;

// Component properties: the knobs a component exposes and an instance turns.
//
// Before this, an instance that wanted a different label or a different colour had two answers:
// an override on the node inside the component, which is a private edit nobody else can see the
// shape of, or a fork of the whole component, which stops receiving fixes. A property is the third
// answer — the component says what may vary, and every instance answers the same question.

namespace {

    // A Button whose label is a property and whose fill depends on a variant.
    struct Fixture {
        Document doc;
        Uuid component, label, instance;
    };

    Fixture BuildButton() {
        Fixture f;
        f.component = f.doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Button");
        f.doc.SetProp(f.component, Prop::Fill, Color{ 0.2f, 0.4f, 0.9f, 1.0f });
        f.label = f.doc.CreateNode(NodeKind::Text, f.component, "Label");
        f.doc.SetProp(f.label, Prop::Text, Binding{ "label" });
        f.doc.MakeComponent(f.component, "Button");

        ComponentProperty text;
        text.name = "label";
        text.type = ValueType::Text;
        text.defaultValue = std::string("Button");
        f.doc.SetComponentProperty(f.component, text);

        ComponentProperty tone;
        tone.name = "tone";
        tone.type = ValueType::Text;
        tone.defaultValue = std::string("primary");
        tone.options = { "primary", "danger" };
        f.doc.SetComponentProperty(f.component, tone);

        // What danger means, said once on the node it applies to.
        f.doc.SetProp(f.component, VariantOverlayPrefix("tone", "danger") + PropName(Prop::Fill),
                      Color{ 0.9f, 0.25f, 0.25f, 1.0f });

        const Uuid screen = f.doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
        f.instance = f.doc.CreateInstance(f.component, screen);
        return f;
    }

}

TEST(variants, a_component_declares_what_may_vary) {
    Fixture f = BuildButton();
    CHECK_EQ(f.doc.PropertiesOf(f.component).size(), std::size_t(2));

    const ComponentProperty* tone = f.doc.FindProperty(f.component, "tone");
    CHECK(tone != nullptr);
    if (tone) {
        CHECK(tone->IsVariant());
        CHECK_EQ(tone->options.size(), std::size_t(2));
    }
    const ComponentProperty* label = f.doc.FindProperty(f.component, "label");
    CHECK(label != nullptr);
    if (label) CHECK(!label->IsVariant());
}

TEST(variants, an_instance_that_says_nothing_gets_the_default) {
    Fixture f = BuildButton();
    // Bound to a local first: InstanceProperty returns a Value by value, and std::get on the
    // temporary hands back a reference into something that is already gone.
    const Value fallback = f.doc.InstanceProperty(f.instance, "label");
    CHECK_EQ(std::get<std::string>(fallback), std::string("Button"));

    // The label node's text is a binding that names the property, so what it resolves to is the
    // default rather than the binding itself.
    const PropBag bag = f.doc.ResolvedProps(f.instance, f.label);
    CHECK_EQ(bag.Text(Prop::Text), std::string("Button"));
}

TEST(variants, an_instance_picks_a_value_and_the_binding_answers_it) {
    Fixture f = BuildButton();
    f.doc.SetInstanceProperty(f.instance, "label", std::string("Save"));

    const Value picked = f.doc.InstanceProperty(f.instance, "label");
    CHECK_EQ(std::get<std::string>(picked), std::string("Save"));
    CHECK_EQ(f.doc.ResolvedProps(f.instance, f.label).Text(Prop::Text), std::string("Save"));

    // And the component itself is untouched: this is one instance's answer, not an edit.
    CHECK(std::holds_alternative<Binding>(*f.doc.Find(f.label)->props.Find(Prop::Text)));
}

TEST(variants, two_instances_of_one_component_answer_differently) {
    Fixture f = BuildButton();
    const Uuid screen = f.doc.Find(f.instance)->parent;
    const Uuid second = f.doc.CreateInstance(f.component, screen);

    f.doc.SetInstanceProperty(f.instance, "label", std::string("Save"));
    f.doc.SetInstanceProperty(second, "label", std::string("Cancel"));

    CHECK_EQ(f.doc.ResolvedProps(f.instance, f.label).Text(Prop::Text), std::string("Save"));
    CHECK_EQ(f.doc.ResolvedProps(second, f.label).Text(Prop::Text), std::string("Cancel"));
}

TEST(variants, a_variant_switches_the_properties_it_names) {
    Fixture f = BuildButton();

    const Color primary = f.doc.ResolvedProps(std::vector<Uuid>{}, f.instance).Colour(Prop::Fill);
    CHECK_NEAR(primary.b, 0.9f);

    f.doc.SetInstanceProperty(f.instance, "tone", std::string("danger"));
    const Color danger = f.doc.ResolvedProps(std::vector<Uuid>{}, f.instance).Colour(Prop::Fill);
    CHECK_NEAR(danger.r, 0.9f);
    CHECK(danger.b < 0.5f);

    // Back to the option that names nothing, and the component's own value is what is left.
    f.doc.SetInstanceProperty(f.instance, "tone", std::string("primary"));
    CHECK_NEAR(f.doc.ResolvedProps(std::vector<Uuid>{}, f.instance).Colour(Prop::Fill).b, 0.9f);
}

TEST(variants, an_override_still_beats_a_variant) {
    Fixture f = BuildButton();
    f.doc.SetInstanceProperty(f.instance, "tone", std::string("danger"));
    // An instance that was hand-recoloured keeps that colour: a variant is a default for the
    // instance, not a rule over it.
    f.doc.SetOverride(f.instance, f.component, Prop::Fill, Color{ 0.0f, 1.0f, 0.0f, 1.0f });

    const Color fill = f.doc.ResolvedProps(std::vector<Uuid>{}, f.instance).Colour(Prop::Fill);
    CHECK_NEAR(fill.g, 1.0f);
}

TEST(variants, a_binding_that_names_nothing_is_left_for_the_runtime) {
    Fixture f = BuildButton();
    const Uuid other = f.doc.CreateNode(NodeKind::Text, f.component, "Count");
    f.doc.SetProp(other, Prop::Text, Binding{ "cart.items" });

    // Not a component property, so it stays a binding — the runtime is the only thing that can
    // evaluate it, and answering it here with nothing would erase it.
    const PropBag bag = f.doc.ResolvedProps(f.instance, other);
    const Value* text = bag.Find(Prop::Text);
    CHECK(text != nullptr);
    if (text) CHECK(std::holds_alternative<Binding>(*text));
}

TEST(variants, properties_and_the_values_picked_survive_a_save) {
    Fixture f = BuildButton();
    f.doc.SetInstanceProperty(f.instance, "label", std::string("Save"));
    f.doc.SetInstanceProperty(f.instance, "tone", std::string("danger"));

    const std::string xml = Serializer::ToXml(f.doc, true, &ui::StandardLibrary(), true);
    CHECK(xml.find("<property") != std::string::npos);
    CHECK(xml.find("options=\"primary,danger\"") != std::string::npos);

    Document back;
    std::string error;
    CHECK(Serializer::FromXml(xml, back, &error, &ui::StandardLibrary()));
    CHECK(error.empty());

    CHECK_EQ(back.PropertiesOf(f.component).size(), std::size_t(2));
    const ComponentProperty* tone = back.FindProperty(f.component, "tone");
    CHECK(tone != nullptr);
    if (tone) {
        CHECK_EQ(tone->options.size(), std::size_t(2));
        CHECK_EQ(tone->options[1], std::string("danger"));
        CHECK_EQ(std::get<std::string>(tone->defaultValue), std::string("primary"));
    }

    const Value reloaded = back.InstanceProperty(f.instance, "label");
    CHECK_EQ(std::get<std::string>(reloaded), std::string("Save"));
    CHECK_EQ(back.ResolvedProps(f.instance, f.label).Text(Prop::Text), std::string("Save"));
    CHECK_NEAR(back.ResolvedProps(std::vector<Uuid>{}, f.instance).Colour(Prop::Fill).r, 0.9f);
}
