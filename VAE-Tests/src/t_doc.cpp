#include "Test.h"

#include "vae/doc/Command.h"
#include "vae/doc/Serializer.h"

#include <string>
#include <vector>

using namespace vae;
using namespace vae::doc;

// ------------------------------------------------------------------ structure

TEST(doc, creates_and_finds_nodes) {
    Document doc;
    const Uuid root = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid child = doc.CreateNode(NodeKind::Frame, root, "Card");

    CHECK_EQ(doc.NodeCount(), 2u);
    CHECK_EQ(doc.Roots().size(), 1u);
    CHECK(doc.Find(child) != nullptr);
    CHECK_EQ(doc.Find(child)->name, std::string("Card"));
    CHECK_EQ(doc.Find(root)->children.size(), 1u);
}

TEST(doc, deleting_a_node_takes_its_whole_subtree) {
    Document doc;
    const Uuid root = doc.CreateNode(NodeKind::Frame);
    const Uuid mid = doc.CreateNode(NodeKind::Frame, root);
    doc.CreateNode(NodeKind::Frame, mid);
    doc.CreateNode(NodeKind::Frame, mid);
    CHECK_EQ(doc.NodeCount(), 4u);

    doc.DeleteNode(mid);
    CHECK_EQ(doc.NodeCount(), 1u);
    CHECK_EQ(doc.Find(root)->children.size(), 0u);
}

TEST(doc, subtree_returns_parents_before_children) {
    Document doc;
    const Uuid root = doc.CreateNode(NodeKind::Frame);
    const Uuid a = doc.CreateNode(NodeKind::Frame, root);
    const Uuid b = doc.CreateNode(NodeKind::Frame, a);

    const auto subtree = doc.Subtree(root);
    CHECK_EQ(subtree.size(), 3u);
    CHECK(subtree[0] == root);
    CHECK(subtree[1] == a);
    CHECK(subtree[2] == b);
}

TEST(doc, reparenting_into_own_subtree_is_refused) {
    Document doc;
    const Uuid root = doc.CreateNode(NodeKind::Frame);
    const Uuid child = doc.CreateNode(NodeKind::Frame, root);

    doc.Reparent(root, child, 0);          // would orphan the whole branch
    CHECK(doc.Find(root)->parent == Uuid::Invalid());
    CHECK(doc.Find(child)->parent == root);
}

TEST(doc, reorder_moves_a_node_among_its_siblings) {
    Document doc;
    const Uuid root = doc.CreateNode(NodeKind::Frame);
    const Uuid a = doc.CreateNode(NodeKind::Frame, root, "a");
    const Uuid b = doc.CreateNode(NodeKind::Frame, root, "b");
    const Uuid c = doc.CreateNode(NodeKind::Frame, root, "c");

    CHECK_EQ(doc.IndexInParent(c), 2u);
    doc.Reorder(c, 0);
    CHECK_EQ(doc.IndexInParent(c), 0u);
    CHECK_EQ(doc.IndexInParent(a), 1u);
    CHECK_EQ(doc.IndexInParent(b), 2u);
}

TEST(doc, observers_see_every_change) {
    Document doc;
    int notifications = 0;
    const u32 handle = doc.AddObserver([&](Uuid) { ++notifications; });

    const Uuid node = doc.CreateNode(NodeKind::Frame);
    doc.SetProp(node, Prop::Opacity, 0.5f);
    CHECK_EQ(notifications, 2);

    doc.RemoveObserver(handle);
    doc.SetProp(node, Prop::Opacity, 0.25f);
    CHECK_EQ(notifications, 2);
}

// ------------------------------------------------------------------ properties + tokens

TEST(doc, unset_is_distinguishable_from_a_default) {
    PropBag bag;
    CHECK(!bag.Has(Prop::Opacity));
    CHECK_NEAR(bag.Number(Prop::Opacity, 1.0f), 1.0f);

    bag.Set(Prop::Opacity, 0.0f);
    CHECK(bag.Has(Prop::Opacity));          // explicitly zero, not absent
    CHECK_NEAR(bag.Number(Prop::Opacity, 1.0f), 0.0f);

    bag.Unset(Prop::Opacity);
    CHECK(!bag.Has(Prop::Opacity));
}

TEST(doc, tokens_resolve_per_theme) {
    Document doc;
    Token surface;
    surface.light = Color{ 1.0f, 1.0f, 1.0f, 1.0f };
    surface.dark  = Color{ 0.1f, 0.1f, 0.1f, 1.0f };
    doc.SetToken("surface", surface);

    doc.SetTheme(Theme::Dark);
    const Color dark = std::get<Color>(doc.ResolveValue(TokenRef{ "surface" }));
    CHECK_NEAR(dark.r, 0.1f);

    doc.SetTheme(Theme::Light);
    const Color light = std::get<Color>(doc.ResolveValue(TokenRef{ "surface" }));
    CHECK_NEAR(light.r, 1.0f);
}

TEST(doc, a_token_may_alias_another_token) {
    Document doc;
    Token base;
    base.light = base.dark = Color{ 0.5f, 0.0f, 0.0f, 1.0f };
    doc.SetToken("red", base);

    Token alias;
    alias.light = alias.dark = TokenRef{ "red" };
    doc.SetToken("danger", alias);

    const Color resolved = std::get<Color>(doc.ResolveValue(TokenRef{ "danger" }));
    CHECK_NEAR(resolved.r, 0.5f);
}

TEST(doc, an_undefined_token_resolves_to_unset_rather_than_garbage) {
    Document doc;
    CHECK(!IsSet(doc.ResolveValue(TokenRef{ "nope" })));
}

// ------------------------------------------------------------------ components

TEST(doc, instance_inherits_component_properties) {
    Document doc;
    const Uuid component = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Button");
    const Uuid label = doc.CreateNode(NodeKind::Text, component, "Label");
    doc.SetProp(label, Prop::Text, std::string("Click me"));
    doc.MakeComponent(component, "Button");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid instance = doc.CreateInstance(component, screen);
    CHECK(instance.Valid());

    const PropBag resolved = doc.ResolvedProps(instance, label);
    CHECK_EQ(resolved.Text(Prop::Text), std::string("Click me"));
}

TEST(doc, an_override_beats_the_component_default) {
    Document doc;
    const Uuid component = doc.CreateNode(NodeKind::Frame);
    const Uuid label = doc.CreateNode(NodeKind::Text, component);
    doc.SetProp(label, Prop::Text, std::string("Default"));
    doc.MakeComponent(component, "Button");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid a = doc.CreateInstance(component, screen);
    const Uuid b = doc.CreateInstance(component, screen);
    doc.SetOverride(a, label, Prop::Text, std::string("Save"));

    CHECK_EQ(doc.ResolvedProps(a, label).Text(Prop::Text), std::string("Save"));
    CHECK_EQ(doc.ResolvedProps(b, label).Text(Prop::Text), std::string("Default"));

    // Editing the master propagates to the instance that did not override it, and not to the one
    // that did — the whole point of the component model.
    doc.SetProp(label, Prop::Text, std::string("Changed"));
    CHECK_EQ(doc.ResolvedProps(a, label).Text(Prop::Text), std::string("Save"));
    CHECK_EQ(doc.ResolvedProps(b, label).Text(Prop::Text), std::string("Changed"));
}

TEST(doc, clearing_an_override_falls_back_to_the_component) {
    Document doc;
    const Uuid component = doc.CreateNode(NodeKind::Frame);
    const Uuid label = doc.CreateNode(NodeKind::Text, component);
    doc.SetProp(label, Prop::Text, std::string("Default"));
    doc.MakeComponent(component, "Button");

    const Uuid instance = doc.CreateInstance(component, doc.CreateNode(NodeKind::Screen));
    doc.SetOverride(instance, label, Prop::Text, std::string("Custom"));
    doc.ClearOverride(instance, label, Prop::Text);
    CHECK_EQ(doc.ResolvedProps(instance, label).Text(Prop::Text), std::string("Default"));
}

TEST(doc, flatten_expands_instances_into_a_render_tree) {
    Document doc;
    const Uuid component = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Row");
    doc.CreateNode(NodeKind::Text, component, "Label");
    doc.CreateNode(NodeKind::Image, component, "Icon");
    doc.MakeComponent(component, "Row");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    doc.CreateInstance(component, screen);
    doc.CreateInstance(component, screen);

    const auto flat = doc.Flatten(screen);
    // screen + 2 instances, each expanding to itself plus 2 children
    CHECK_EQ(flat.size(), 7u);

    u32 texts = 0;
    for (const auto& node : flat) if (node.kind == NodeKind::Text) ++texts;
    CHECK_EQ(texts, 2u);
}

TEST(doc, flattened_instance_children_carry_their_overrides) {
    Document doc;
    const Uuid component = doc.CreateNode(NodeKind::Frame);
    const Uuid label = doc.CreateNode(NodeKind::Text, component);
    doc.SetProp(label, Prop::Text, std::string("Default"));
    doc.MakeComponent(component, "Button");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid instance = doc.CreateInstance(component, screen);
    doc.SetOverride(instance, label, Prop::Text, std::string("Overridden"));

    const auto flat = doc.Flatten(screen);
    bool found = false;
    for (const auto& node : flat)
        if (node.kind == NodeKind::Text) {
            CHECK_EQ(node.props.Text(Prop::Text), std::string("Overridden"));
            found = true;
        }
    CHECK(found);
}

TEST(doc, an_outer_instance_can_override_a_node_two_components_deep) {
    // Components made of components is the whole point of a catalog: a Card holds a Button, a Button
    // holds a Label. Two Cards on a screen are two Cards, so retitling one must not retitle both —
    // and the node being retitled belongs to neither Card but to the Button inside them.
    Document doc;

    const Uuid buttonRoot = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Button");
    const Uuid label = doc.CreateNode(NodeKind::Text, buttonRoot, "Label");
    doc.SetProp(label, Prop::Text, std::string("Default"));
    const Uuid button = doc.MakeComponent(buttonRoot, "Button");

    const Uuid cardRoot = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Card");
    doc.CreateInstance(button, cardRoot);
    const Uuid card = doc.MakeComponent(cardRoot, "Card");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid first  = doc.CreateInstance(card, screen);
    doc.CreateInstance(card, screen);

    // The override is written on the instance that is actually on the screen — the only node a
    // designer selected — and keyed by the deep node it is about.
    doc.SetOverride(first, label, Prop::Text, std::string("Only this one"));

    std::vector<std::string> texts;
    for (const auto& node : doc.Flatten(screen))
        if (node.kind == NodeKind::Text) texts.push_back(node.props.Text(Prop::Text));

    CHECK_EQ(texts.size(), 2u);
    if (texts.size() == 2) {
        CHECK_EQ(texts[0], std::string("Only this one"));
        CHECK_EQ(texts[1], std::string("Default"));
    }
}

TEST(doc, an_outer_override_beats_one_baked_into_the_component) {
    // The Card's author already said what its Button reads; the designer placing the Card gets the
    // last word, or "override" would mean "unless someone got there first".
    Document doc;

    const Uuid buttonRoot = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Button");
    const Uuid label = doc.CreateNode(NodeKind::Text, buttonRoot, "Label");
    doc.SetProp(label, Prop::Text, std::string("Default"));
    const Uuid button = doc.MakeComponent(buttonRoot, "Button");

    const Uuid cardRoot = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Card");
    const Uuid inner = doc.CreateInstance(button, cardRoot);
    doc.SetOverride(inner, label, Prop::Text, std::string("From the card"));
    const Uuid card = doc.MakeComponent(cardRoot, "Card");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid outer = doc.CreateInstance(card, screen);

    for (const auto& node : doc.Flatten(screen))
        if (node.kind == NodeKind::Text)
            CHECK_EQ(node.props.Text(Prop::Text), std::string("From the card"));

    doc.SetOverride(outer, label, Prop::Text, std::string("From the screen"));
    for (const auto& node : doc.Flatten(screen))
        if (node.kind == NodeKind::Text)
            CHECK_EQ(node.props.Text(Prop::Text), std::string("From the screen"));
}

TEST(doc, a_grid_survives_a_round_trip_through_the_serializer) {
    // Grid is the first layout mode with fields of its own, and a mode the file cannot carry is a
    // mode the designer loses on save.
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    Node* node = doc.Find(screen);
    node->layout.mode = layout::LayoutMode::Grid;
    node->layout.columns = 4;
    node->layout.minColumn = 180.0f;
    node->layout.rowGap = 12.0f;
    node->layout.gap = 6.0f;
    doc.Touch(screen);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromJson(Serializer::ToJson(doc, false), loaded, &error), error);

    const Node* back = loaded.Find(screen);
    CHECK(back != nullptr);
    if (!back) return;
    CHECK(back->layout.mode == layout::LayoutMode::Grid);
    CHECK(back->layout.columns == 4);
    CHECK_NEAR(back->layout.minColumn, 180.0f);
    CHECK_NEAR(back->layout.rowGap, 12.0f);
}

TEST(doc, a_component_cannot_be_instantiated_inside_itself) {
    Document doc;
    const Uuid component = doc.CreateNode(NodeKind::Frame);
    const Uuid inner = doc.CreateNode(NodeKind::Frame, component);
    doc.MakeComponent(component, "Recursive");

    CHECK(!doc.CreateInstance(component, inner).Valid());
    CHECK(!doc.CreateInstance(component, component).Valid());
}

// ------------------------------------------------------------------ commands

TEST(command, set_prop_undoes_and_redoes) {
    Document doc;
    CommandStack stack;
    const Uuid node = doc.CreateNode(NodeKind::Frame);

    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity, Value{ 0.5f }));
    CHECK_NEAR(std::get<f32>(doc.GetProp(node, Prop::Opacity)), 0.5f);

    CHECK(stack.Undo(doc));
    CHECK(!IsSet(doc.GetProp(node, Prop::Opacity)));

    CHECK(stack.Redo(doc));
    CHECK_NEAR(std::get<f32>(doc.GetProp(node, Prop::Opacity)), 0.5f);
}

TEST(command, a_drag_coalesces_into_one_undo_entry) {
    Document doc;
    CommandStack stack;
    const Uuid node = doc.CreateNode(NodeKind::Frame);
    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity, Value{ 0.0f }));
    stack.Break();

    for (int i = 1; i <= 20; ++i)
        stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity,
                                                       Value{ static_cast<f32>(i) / 20.0f }));

    CHECK_EQ(stack.UndoDepth(), 2u);        // the initial set, plus the whole drag
    CHECK_NEAR(std::get<f32>(doc.GetProp(node, Prop::Opacity)), 1.0f);

    // Undo must go back to where the drag STARTED, not to the previous mouse position.
    stack.Undo(doc);
    CHECK_NEAR(std::get<f32>(doc.GetProp(node, Prop::Opacity)), 0.0f);
}

TEST(command, break_ends_a_coalescing_run) {
    Document doc;
    CommandStack stack;
    const Uuid node = doc.CreateNode(NodeKind::Frame);

    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity, Value{ 0.2f }));
    stack.Break();
    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity, Value{ 0.8f }));
    CHECK_EQ(stack.UndoDepth(), 2u);
}

TEST(command, different_properties_do_not_coalesce) {
    Document doc;
    CommandStack stack;
    const Uuid node = doc.CreateNode(NodeKind::Frame);

    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity, Value{ 0.2f }));
    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::StrokeWidth, Value{ 2.0f }));
    CHECK_EQ(stack.UndoDepth(), 2u);
}

TEST(command, delete_and_undo_restores_the_subtree_with_identical_ids) {
    Document doc;
    CommandStack stack;
    const Uuid root = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "root");
    const Uuid a = doc.CreateNode(NodeKind::Frame, root, "a");
    const Uuid b = doc.CreateNode(NodeKind::Text, a, "b");
    doc.SetProp(b, Prop::Text, std::string("hello"));

    stack.Execute(doc, CreateScope<DeleteNodeCommand>(a));
    CHECK_EQ(doc.NodeCount(), 1u);

    CHECK(stack.Undo(doc));
    CHECK_EQ(doc.NodeCount(), 3u);
    CHECK(doc.Contains(a));
    CHECK(doc.Contains(b));                                  // same ids, so references survive
    CHECK_EQ(doc.Find(b)->name, std::string("b"));
    CHECK_EQ(doc.Find(b)->props.Text(Prop::Text), std::string("hello"));
    CHECK(doc.Find(a)->parent == root);
    CHECK_EQ(doc.Find(a)->children.size(), 1u);
}

TEST(command, create_redo_reuses_the_same_id) {
    Document doc;
    CommandStack stack;
    auto command = CreateScope<CreateNodeCommand>(NodeKind::Frame, Uuid::Invalid(), "n");
    CreateNodeCommand* raw = command.get();
    stack.Execute(doc, std::move(command));

    const Uuid created = raw->Created();
    stack.Undo(doc);
    CHECK(!doc.Contains(created));
    stack.Redo(doc);
    CHECK(doc.Contains(created));
}

TEST(command, reparent_undoes_to_the_original_slot) {
    Document doc;
    CommandStack stack;
    const Uuid a = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "a");
    const Uuid b = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "b");
    const Uuid x = doc.CreateNode(NodeKind::Frame, a, "x0");
    doc.CreateNode(NodeKind::Frame, a, "x1");

    stack.Execute(doc, CreateScope<ReparentCommand>(x, b, 0));
    CHECK(doc.Find(x)->parent == b);

    stack.Undo(doc);
    CHECK(doc.Find(x)->parent == a);
    CHECK_EQ(doc.IndexInParent(x), 0u);
}

TEST(command, a_transaction_undoes_as_one_step) {
    Document doc;
    CommandStack stack;
    const Uuid node = doc.CreateNode(NodeKind::Frame);

    stack.BeginTransaction("Style node");
    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity, Value{ 0.5f }));
    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::StrokeWidth, Value{ 3.0f }));
    stack.Execute(doc, CreateScope<RenameCommand>(node, "Styled"));
    stack.EndTransaction(doc);

    CHECK_EQ(stack.UndoDepth(), 1u);
    CHECK_EQ(stack.UndoName(), std::string_view("Style node"));

    stack.Undo(doc);
    CHECK(!IsSet(doc.GetProp(node, Prop::Opacity)));
    CHECK(!IsSet(doc.GetProp(node, Prop::StrokeWidth)));
    CHECK_EQ(doc.Find(node)->name, std::string("frame"));
}

TEST(command, executing_after_undo_clears_the_redo_stack) {
    Document doc;
    CommandStack stack;
    const Uuid node = doc.CreateNode(NodeKind::Frame);

    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::Opacity, Value{ 0.5f }));
    stack.Undo(doc);
    CHECK(stack.CanRedo());

    stack.Execute(doc, CreateScope<SetPropCommand>(node, Prop::StrokeWidth, Value{ 1.0f }));
    CHECK(!stack.CanRedo());
}

TEST(command, history_is_bounded) {
    Document doc;
    CommandStack stack;
    stack.SetLimit(10);
    const Uuid node = doc.CreateNode(NodeKind::Frame);

    for (int i = 0; i < 50; ++i) {
        stack.Execute(doc, CreateScope<RenameCommand>(node, "n" + std::to_string(i)));
        stack.Break();
    }
    CHECK_EQ(stack.UndoDepth(), 10u);
}

// ------------------------------------------------------------------ serialization

TEST(serializer, round_trips_a_document) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid card = doc.CreateNode(NodeKind::Frame, screen, "Card");
    const Uuid label = doc.CreateNode(NodeKind::Text, card, "Title");

    doc.SetProp(card, Prop::Fill, Color{ 0.1f, 0.2f, 0.3f, 1.0f });
    doc.SetProp(card, Prop::CornerRadius, 12.0f);
    doc.SetProp(label, Prop::Text, std::string("Hello, world"));
    doc.SetProp(label, Prop::TextColor, TokenRef{ "text.primary" });

    layout::LayoutStyle style;
    style.mode = layout::LayoutMode::Stack;
    style.axis = layout::Axis::Row;
    style.width = layout::Size::Fill();
    style.height = layout::Size::Px(64.0f);
    style.padding = Edges{ 8.0f, 4.0f, 8.0f, 4.0f };
    style.gap = 6.0f;
    style.wrap = true;
    style.align = layout::Align::Center;
    doc.Find(card)->layout = style;

    Token token;
    token.light = Color{ 0.0f, 0.0f, 0.0f, 1.0f };
    token.dark  = Color{ 1.0f, 1.0f, 1.0f, 1.0f };
    doc.SetToken("text.primary", token);

    const std::string json = Serializer::ToJson(doc);

    Document loaded;
    std::string error;
    CHECK(Serializer::FromJson(json, loaded, &error));
    CHECK_EQ(error, std::string(""));

    CHECK_EQ(loaded.NodeCount(), 3u);
    CHECK_EQ(loaded.Roots().size(), 1u);
    CHECK(loaded.Contains(card));
    CHECK_EQ(loaded.Find(card)->children.size(), 1u);
    CHECK_EQ(loaded.Find(label)->props.Text(Prop::Text), std::string("Hello, world"));
    CHECK_NEAR(loaded.Find(card)->props.Number(Prop::CornerRadius), 12.0f);

    const auto& restored = loaded.Find(card)->layout;
    CHECK(restored.mode == layout::LayoutMode::Stack);
    CHECK(restored.axis == layout::Axis::Row);
    CHECK(restored.width.mode == layout::SizeMode::Fill);
    CHECK_NEAR(restored.height.value, 64.0f);
    CHECK_NEAR(restored.gap, 6.0f);
    CHECK(restored.wrap);
    CHECK(restored.align == layout::Align::Center);

    // Unbounded max size survives the JSON trip (infinity has no JSON literal).
    CHECK(!std::isfinite(restored.maxSize.x));

    const auto* loadedToken = loaded.FindToken("text.primary");
    CHECK(loadedToken != nullptr);
    CHECK_NEAR(std::get<Color>(loadedToken->dark).r, 1.0f);

    // A token reference must still be a token reference, not a string that looks like one.
    CHECK(TypeOf(*loaded.Find(label)->props.Find(Prop::TextColor)) == ValueType::Token);
}

TEST(serializer, round_trips_components_and_overrides) {
    Document doc;
    const Uuid component = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Button");
    const Uuid label = doc.CreateNode(NodeKind::Text, component, "Label");
    doc.SetProp(label, Prop::Text, std::string("Default"));
    doc.MakeComponent(component, "Button");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid instance = doc.CreateInstance(component, screen);
    doc.SetOverride(instance, label, Prop::Text, std::string("Save"));

    Document loaded;
    CHECK(Serializer::FromJson(Serializer::ToJson(doc), loaded));
    CHECK(loaded.Find(instance)->componentId == component);
    CHECK_EQ(loaded.ResolvedProps(instance, label).Text(Prop::Text), std::string("Save"));
}

TEST(serializer, refuses_a_document_from_a_newer_build) {
    Document doc;
    std::string json = Serializer::ToJson(doc);
    // Forge a future version; the reader must refuse rather than half-read it.
    const auto pos = json.find("\"version\": 1");
    CHECK(pos != std::string::npos);
    json.replace(pos, std::string("\"version\": 1").size(), "\"version\": 99");

    Document loaded;
    std::string error;
    CHECK(!Serializer::FromJson(json, loaded, &error));
    CHECK(error.find("newer") != std::string::npos);
}

TEST(serializer, rejects_junk_without_crashing) {
    Document loaded;
    std::string error;
    CHECK(!Serializer::FromJson("not json at all", loaded, &error));
    CHECK(!Serializer::FromJson("{}", loaded, &error));
    CHECK(!Serializer::FromJson(R"({"format":"something.else","version":1})", loaded, &error));
}

TEST(serializer, missing_optional_fields_take_their_defaults) {
    // A minimal hand-written document: no layout block, no props, no children.
    const std::string json = R"({
        "format": "vae.document",
        "version": 1,
        "roots": ["0000000000000001"],
        "nodes": [ { "id": "0000000000000001", "kind": "frame", "name": "Bare" } ]
    })";

    Document loaded;
    std::string error;
    CHECK(Serializer::FromJson(json, loaded, &error));
    const Node* node = loaded.Find(Uuid(1));
    CHECK(node != nullptr);
    CHECK_EQ(node->name, std::string("Bare"));
    CHECK(node->visible);
    CHECK(node->layout.width.mode == layout::SizeMode::Hug);
}

TEST(serializer, project_files_round_trip) {
    Project project;
    project.name = "Demo";
    project.scriptLanguage = "cpp";
    project.screens = { "screens/home.vaescreen", "screens/settings.vaescreen" };
    project.components = { "components/button.vaecomp" };
    project.targetResolution = { 1440.0f, 900.0f };

    const auto path = std::filesystem::temp_directory_path() / "vae-test-project.vaeproj";
    CHECK(Project::Save(project, path));

    Project loaded;
    std::string error;
    CHECK(Project::Load(path, loaded, &error));
    CHECK_EQ(loaded.name, std::string("Demo"));
    CHECK_EQ(loaded.scriptLanguage, std::string("cpp"));
    CHECK_EQ(loaded.screens.size(), 2u);
    CHECK_NEAR(loaded.targetResolution.x, 1440.0f);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(doc, assets_are_kept_by_id_and_survive_a_round_trip) {
    Document document;
    const Uuid logo = document.AddAsset("logo", "assets/logo.png");
    const Uuid hero = document.AddAsset("hero", "assets/hero.jpg");
    CHECK(logo.Valid() && hero.Valid() && logo != hero);
    CHECK_EQ(document.Assets().size(), std::size_t(2));

    const Uuid node = document.CreateNode(NodeKind::Image, Uuid::Invalid(), "Picture");
    document.SetProp(node, Prop::Image, AssetRef{ logo });

    // Re-adding the same id renames rather than duplicating: a re-import of a file already in the
    // project must not leave every node pointing at the old copy.
    document.AddAsset("logo mark", "assets/logo-2.png", logo);
    CHECK_EQ(document.Assets().size(), std::size_t(2));
    CHECK(document.FindAsset(logo) != nullptr);
    CHECK_EQ(document.FindAsset(logo)->path, std::string("assets/logo-2.png"));

    const std::string json = Serializer::ToJson(document);
    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromJson(json, loaded, &error), error);
    CHECK_EQ(loaded.Assets().size(), std::size_t(2));
    CHECK(loaded.FindAsset(hero) != nullptr);
    CHECK(loaded.FindAsset(hero) && loaded.FindAsset(hero)->name == "hero");

    // And the node still points at the same asset, which is the whole reason it is an id.
    const Value ref = loaded.GetProp(node, Prop::Image);
    CHECK(std::holds_alternative<AssetRef>(ref));
    CHECK(std::holds_alternative<AssetRef>(ref) && std::get<AssetRef>(ref).id == logo);

    loaded.RemoveAsset(logo);
    CHECK_EQ(loaded.Assets().size(), std::size_t(1));
    CHECK(loaded.FindAsset(logo) == nullptr);
}
