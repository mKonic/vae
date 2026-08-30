#include "Test.h"

#include "vae/base/FileSystem.h"
#include "vae/doc/Command.h"
#include "vae/doc/Serializer.h"
#include "vae/doc/Strings.h"
#include "vae/ui/Library.h"

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
    CHECK_MESSAGE(Serializer::FromXml(Serializer::ToXml(doc, false, nullptr, true), loaded, &error), error);

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

TEST(command, an_asset_comes_back_with_the_id_it_had) {
    // Nodes point at assets by id. An undo that restored the asset under a fresh id would leave
    // every picture that used it blank, which is worse than not undoing at all.
    Document doc;
    CommandStack stack;

    auto add = CreateScope<AddAssetCommand>("logo", "assets/logo.png");
    AddAssetCommand* added = add.get();
    stack.Execute(doc, std::move(add));
    const Uuid id = added->Created();
    CHECK(id.Valid());
    CHECK_EQ(doc.Assets().size(), 1u);

    const Uuid picture = doc.CreateNode(NodeKind::Image, Uuid::Invalid(), "Logo");
    doc.SetProp(picture, Prop::Image, AssetRef{ id });

    stack.Undo(doc);
    CHECK(doc.Assets().empty());
    stack.Redo(doc);
    CHECK_EQ(doc.Assets().size(), 1u);
    CHECK_EQ(doc.Assets()[0].id, id);                 // the same id, so the node still resolves
    CHECK(doc.FindAsset(id) != nullptr);

    // And removing one is undoable in the same way.
    stack.Execute(doc, CreateScope<RemoveAssetCommand>(id));
    CHECK(doc.Assets().empty());
    stack.Undo(doc);
    CHECK_EQ(doc.Assets().size(), 1u);
    CHECK_EQ(doc.Assets()[0].id, id);
    CHECK_EQ(doc.Assets()[0].name, std::string("logo"));
    CHECK_EQ(doc.Assets()[0].path, std::string("assets/logo.png"));
}

TEST(command, the_start_screen_and_the_theme_are_edits_too) {
    // Both are written into the file, and anything written into the file has to be undoable.
    Document doc;
    CommandStack stack;
    const Uuid home = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid detail = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Detail");
    doc.SetStartScreen(home);

    stack.Execute(doc, CreateScope<SetStartScreenCommand>(detail));
    CHECK_EQ(doc.StartScreen(), detail);
    stack.Undo(doc);
    CHECK_EQ(doc.StartScreen(), home);

    CHECK(doc.ActiveTheme() == Theme::Dark);
    stack.Execute(doc, CreateScope<SetThemeCommand>(Theme::Light));
    CHECK(doc.ActiveTheme() == Theme::Light);
    stack.Undo(doc);
    CHECK(doc.ActiveTheme() == Theme::Dark);
    stack.Redo(doc);
    CHECK(doc.ActiveTheme() == Theme::Light);
}

TEST(command, renaming_a_token_takes_every_reference_with_it) {
    // A rename that left the references behind would silently unstyle half the document — every
    // node that named the old token would resolve to nothing.
    Document doc;
    CommandStack stack;
    doc.SetToken("brand", Token{ Color{ 1, 0, 0, 1 }, Color{ 1, 0, 0, 1 } });

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid card = doc.CreateNode(NodeKind::Frame, screen, "Card");
    doc.SetProp(card, Prop::Fill, TokenRef{ "brand" });
    doc.SetProp(card, "hovered:fill", TokenRef{ "brand" });
    const Uuid other = doc.CreateNode(NodeKind::Frame, screen, "Other");
    doc.SetProp(other, Prop::Fill, TokenRef{ "surface" });

    stack.Execute(doc, CreateScope<RenameTokenCommand>("brand", "accent"));
    CHECK(doc.FindToken("accent") != nullptr);
    CHECK(doc.FindToken("brand") == nullptr);
    CHECK(doc.GetProp(card, Prop::Fill) == Value{ TokenRef{ "accent" } });
    CHECK(*doc.Find(card)->props.Find("hovered:fill") == Value{ TokenRef{ "accent" } });
    // A node that named a different token is left alone.
    CHECK(doc.GetProp(other, Prop::Fill) == Value{ TokenRef{ "surface" } });

    stack.Undo(doc);
    CHECK(doc.FindToken("brand") != nullptr);
    CHECK(doc.GetProp(card, Prop::Fill) == Value{ TokenRef{ "brand" } });
}

TEST(command, adding_and_removing_a_token_is_undoable) {
    Document doc;
    CommandStack stack;

    Token blue;
    blue.dark = blue.light = Color{ 0.2f, 0.4f, 0.9f, 1.0f };
    stack.Execute(doc, CreateScope<SetTokenCommand>("accent", blue));
    CHECK(doc.FindToken("accent") != nullptr);
    stack.Undo(doc);
    CHECK(doc.FindToken("accent") == nullptr);
    stack.Redo(doc);
    CHECK(doc.FindToken("accent") != nullptr);

    // Editing an existing one restores the old value rather than deleting it.
    Token red = blue;
    red.dark = red.light = Color{ 0.9f, 0.2f, 0.2f, 1.0f };
    stack.Execute(doc, CreateScope<SetTokenCommand>("accent", red));
    stack.Undo(doc);
    CHECK(doc.FindToken("accent") != nullptr);
    const Value restored = doc.FindToken("accent")->dark;
    const Value wanted = Value{ Color{ 0.2f, 0.4f, 0.9f, 1.0f } };
    CHECK(restored == wanted);

    stack.Execute(doc, CreateScope<RemoveTokenCommand>("accent"));
    CHECK(doc.FindToken("accent") == nullptr);
    stack.Undo(doc);
    CHECK(doc.FindToken("accent") != nullptr);
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

    const std::string xml = Serializer::ToXml(doc, true, nullptr, true);

    Document loaded;
    std::string error;
    CHECK(Serializer::FromXml(xml, loaded, &error));
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
    CHECK(Serializer::FromXml(Serializer::ToXml(doc, true, nullptr, true), loaded));
    CHECK(loaded.Find(instance)->componentId == component);
    CHECK_EQ(loaded.ResolvedProps(instance, label).Text(Prop::Text), std::string("Save"));
}


TEST(serializer, rejects_junk_without_crashing) {
    Document loaded;
    std::string error;
    CHECK(!Serializer::FromXml("not markup at all", loaded, &error));
    CHECK(!Serializer::FromXml("", loaded, &error));
    CHECK(!Serializer::FromXml("<vae>", loaded, &error));
    CHECK(!Serializer::FromXml("<something version=\"4\"/>", loaded, &error));
    // Well-formed markup that is not a document: a root with no version says nothing about what it
    // is, and guessing is how half a file gets read.
    CHECK(!Serializer::FromXml("<vae><screen name=\"Home\"/></vae>", loaded, &error));
    CHECK(!error.empty());
}

TEST(serializer, missing_optional_fields_take_their_defaults) {
    // A minimal hand-written document: no layout attributes, no properties, no children. Everything
    // the encoder leaves out because it is at its default has to come back as that default.
    const std::string xml =
        "<vae version=\"" + std::to_string(Serializer::kFormatVersion) + "\">"
        "<frame id=\"0000000000000001\" name=\"Bare\"/></vae>";

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    const Node* node = loaded.Find(Uuid(1));
    CHECK(node != nullptr);
    if (!node) return;
    CHECK_EQ(node->name, std::string("Bare"));
    CHECK(node->visible);
    CHECK(!node->locked);
    CHECK(!node->slot);
    CHECK(node->props.Empty());
    CHECK(node->layout == layout::LayoutStyle{});
}

TEST(serializer, project_files_round_trip) {
    Project project;
    project.name = "Demo";
    project.scriptLanguage = "cpp";
    project.fontDirs = { "fonts", "vendor/fonts" };
    project.targetResolution = { 1440.0f, 900.0f };

    const auto dir = std::filesystem::temp_directory_path() / "vae-test-project";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / Project::kFileName;
    CHECK(Project::Save(project, path));

    Project loaded;
    std::string error;
    CHECK_MESSAGE(Project::Load(path, loaded, &error), error);
    CHECK_EQ(loaded.name, std::string("Demo"));
    CHECK_EQ(loaded.scriptLanguage, std::string("cpp"));
    CHECK_EQ(loaded.fontDirs.size(), 2u);
    CHECK_EQ(loaded.fontDirs[1], std::string("vendor/fonts"));
    CHECK_NEAR(loaded.targetResolution.x, 1440.0f);
    CHECK_EQ(loaded.root, dir);

    // It is a document like every other file: markup, with the one format version on it.
    const auto text = FileSystem::ReadText(path);
    CHECK(text.has_value());
    if (text) {
        CHECK(text->find("<vae version=\"" + std::to_string(Serializer::kFormatVersion) + "\"")
              != std::string::npos);
        CHECK(text->find("<project ") != std::string::npos);
        // And it does NOT list the project's files: the folders are what exists.
        CHECK(text->find("screens/") == std::string::npos);
    }

    std::filesystem::remove_all(dir, ec);
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

    const std::string xml = Serializer::ToXml(document, true, nullptr, true);
    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
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

// ------------------------------------------------------------------ rows

TEST(doc, a_string_table_is_what_a_translator_is_handed) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid title = doc.CreateNode(NodeKind::Text, screen, "Title");
    doc.SetProp(title, Prop::Text, std::string("Good morning"));
    doc.SetProp(title, Prop::TextKey, std::string("home.greeting"));
    const Uuid plain = doc.CreateNode(NodeKind::Text, screen, "Plain");
    doc.SetProp(plain, Prop::Text, std::string("not translated"));

    StringTable table;
    table.CollectFrom(doc);
    // Only what opted in, and the authored text is the starting point a translator edits.
    CHECK_EQ(table.Count(), 1u);
    CHECK_EQ(std::string(table.Find("home.greeting")), std::string("Good morning"));
    CHECK(table.Find("nothing.here").empty());

    // Re-collecting keeps the work: adding a screen must not overwrite what was translated.
    table.Set("home.greeting", "Bom dia");
    doc.SetProp(doc.CreateNode(NodeKind::Text, screen, "Second"), Prop::TextKey,
                std::string("home.subtitle"));
    table.CollectFrom(doc);
    CHECK_EQ(table.Count(), 2u);
    CHECK_EQ(std::string(table.Find("home.greeting")), std::string("Bom dia"));
}

TEST(doc, a_translation_round_trips_through_a_file) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vae-strings-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    StringTable written;
    written.SetLocale("pt-BR");
    written.Set("home.greeting", "Bom dia");
    written.Set("home.quote", "aspas \"dentro\" do texto");
    CHECK(written.Save(dir / "pt-BR.json"));

    StringTable read;
    std::string error;
    CHECK_MESSAGE(read.Load(dir / "pt-BR.json", &error), error);
    CHECK_EQ(read.Count(), 2u);
    CHECK_EQ(std::string(read.Find("home.greeting")), std::string("Bom dia"));
    CHECK_EQ(std::string(read.Find("home.quote")), std::string("aspas \"dentro\" do texto"));
    // The name of the file is the locale, so a translator can drop one in without editing it.
    CHECK_EQ(read.Locale(), std::string("pt-BR"));

    const std::vector<std::string> found = LocalesIn(dir);
    CHECK_EQ(found.size(), 1u);
    CHECK_MESSAGE(!found.empty() && found.front() == "pt-BR",
                  "locales found: " + (found.empty() ? std::string("(none)") : found.front()));

    // Something that is not a table says so rather than loading as an empty one.
    FileSystem::WriteText(dir / "broken.json", "[\"not an object\"]");
    StringTable broken;
    CHECK(!broken.Load(dir / "broken.json", &error));
    CHECK(!error.empty());

    std::filesystem::remove_all(dir, ec);
}

TEST(doc, sample_rows_are_a_table_typed_as_text) {
    const RowTable table = ParseRowText("author | body | tint\n"
                                        "  Ada  | Hello there | accent\n"
                                        "Grace  | Hi\n");
    CHECK_EQ(table.columns.size(), 3u);
    CHECK_EQ(table.columns[0], std::string("author"));
    CHECK_EQ(table.Count(), 2u);
    // Cells are trimmed, so a table lined up into columns reads as the values it shows.
    CHECK_EQ(std::string(table.Cell(0, "author")), std::string("Ada"));
    CHECK_EQ(std::string(table.Cell(0, "body")), std::string("Hello there"));
    // A short row pads rather than shifting every later cell one column left.
    CHECK_EQ(std::string(table.Cell(1, "author")), std::string("Grace"));
    CHECK_EQ(std::string(table.Cell(1, "tint")), std::string(""));
    // A long one drops the extra instead of growing a column nothing named.
    CHECK_EQ(ParseRowText("a\n1 | 2 | 3").columns.size(), 1u);
    CHECK_EQ(ParseRowText("a\n1 | 2 | 3").Count(), 1u);
}

TEST(doc, column_names_with_no_rows_under_them_are_not_a_table) {
    // Half-typed is the state the field is in for most of the time anyone spends in it, and a
    // container with no rows should draw the number the designer asked for, not zero copies.
    CHECK_EQ(ParseRowText("author | body").Count(), 0u);
    CHECK(ParseRowText("author | body").columns.empty());
    CHECK(ParseRowText("").columns.empty());
    CHECK(ParseRowText("\n\n").columns.empty());
}

TEST(doc, sample_row_text_survives_the_trip_through_a_table) {
    const std::string text = "author | body\nAda | Hello\nGrace | Hi\n";
    CHECK_EQ(RowText(ParseRowText(text)), text);
    CHECK_EQ(RowText(RowTable{}), std::string(""));
}

TEST(doc, rows_decide_how_many_copies_a_repeated_container_has) {
    // The designer draws one row and says "repeat 2" so the canvas is not empty. The app hands
    // over three rows; three is what there are. Data wins over the placeholder, or every list in
    // every screen would be as long as whatever number somebody typed while drawing it.
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid list = doc.CreateNode(NodeKind::Frame, screen, "Messages");
    doc.SetProp(list, Prop::Repeat, 2.0f);
    const Uuid row = doc.CreateNode(NodeKind::Frame, list, "Message");
    const Uuid body = doc.CreateNode(NodeKind::Text, row, "Body");
    doc.SetProp(body, Prop::Field, std::string("body"));

    RowTable table;
    table.columns = { "body" };
    table.cells = { "one", "two", "three" };

    const auto flat = doc.Flatten(screen, [&](Uuid node, Uuid) -> const RowTable* {
        return node == list ? &table : nullptr;
    });

    // screen + list + three copies of (Message, Body)
    CHECK_EQ(flat.size(), 8u);

    std::vector<std::string> bodies;
    for (const auto& node : flat)
        if (node.kind == NodeKind::Text) bodies.push_back(node.props.Text(Prop::Text));
    CHECK_EQ(bodies.size(), 3u);
    CHECK_EQ(bodies[0], std::string("one"));
    CHECK_EQ(bodies[2], std::string("three"));

    // Every node inside a copy knows which copy it is, not just the copy's root: a click lands on
    // the label, and the label has to be able to say which message it belongs to.
    u32 tagged = 0;
    for (const auto& node : flat)
        if (node.repeated) {
            CHECK(node.row >= 0 && node.row <= 2);
            ++tagged;
        }
    CHECK_EQ(tagged, 6u);

    // Without a table the authored count is still what it was.
    CHECK_EQ(doc.Flatten(screen).size(), 6u);
}

TEST(doc, a_field_binding_fills_the_property_its_column_names) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid list = doc.CreateNode(NodeKind::Frame, screen, "Members");
    const Uuid row = doc.CreateNode(NodeKind::Frame, list, "Member");
    doc.SetProp(row, Prop::Field, std::string("fill:tint"));
    const Uuid name = doc.CreateNode(NodeKind::Text, row, "Name");
    doc.SetProp(name, Prop::Field, std::string("name"));           // bare: the natural property
    const Uuid dot = doc.CreateNode(NodeKind::Frame, row, "Dot");
    doc.SetProp(dot, Prop::Visible, true);
    doc.SetProp(dot, Prop::Field, std::string("visible:online"));
    const Uuid badge = doc.CreateNode(NodeKind::Text, row, "Badge");
    doc.SetProp(badge, Prop::Text, std::string("drawn"));
    doc.SetProp(badge, Prop::Field, std::string("unread"));         // a column the data lacks

    RowTable table;
    table.columns = { "name", "tint", "online" };
    table.cells = { "Ada",   "#ff8800", "true",
                    "Grace", "accent",  "no" };

    const auto flat = doc.Flatten(screen, [&](Uuid node, Uuid) -> const RowTable* {
        return node == list ? &table : nullptr;
    });

    // Copies are named for their place — "Member 1" — and everything inside one keeps the name it
    // was drawn with, so a part of row two is (name, row) away.
    const auto find = [&](std::string_view what, i32 which) -> const Document::FlatNode* {
        for (const auto& node : flat)
            if (node.row == which && node.name.rfind(what, 0) == 0) return &node;
        return nullptr;
    };

    const auto* firstRow = find("Member", 0);
    CHECK(firstRow != nullptr);
    if (firstRow) {
        const Value* fill = firstRow->props.Find(Prop::Fill);
        CHECK(fill && std::holds_alternative<Color>(*fill));
        if (fill && std::holds_alternative<Color>(*fill)) {
            CHECK_NEAR(std::get<Color>(*fill).r, 1.0f);
            CHECK_NEAR(std::get<Color>(*fill).g, 0.53333f);
        }
    }

    // A cell that is not a hex triple is a token name, so a row can pick a colour from the theme.
    const auto* secondRow = find("Member", 1);
    CHECK(secondRow != nullptr);
    if (secondRow) {
        const Value* fill = secondRow->props.Find(Prop::Fill);
        CHECK(fill && std::holds_alternative<TokenRef>(*fill));
        if (fill && std::holds_alternative<TokenRef>(*fill))
            CHECK_EQ(std::get<TokenRef>(*fill).name, std::string("accent"));
    }

    const auto* firstName = find("Name", 0);
    const auto* secondName = find("Name", 1);
    CHECK(firstName && firstName->props.Text(Prop::Text) == "Ada");
    CHECK(secondName && secondName->props.Text(Prop::Text) == "Grace");

    const auto* firstDot = find("Dot", 0);
    const auto* secondDot = find("Dot", 1);
    CHECK(firstDot && firstDot->props.Flag(Prop::Visible, false));
    CHECK(secondDot && !secondDot->props.Flag(Prop::Visible, true));

    // Nothing in the data, nothing drawn: a template bound to a column the rows do not carry is
    // blank, not stuck showing whatever the designer typed into it.
    const auto* firstBadge = find("Badge", 0);
    CHECK(firstBadge && firstBadge->props.Text(Prop::Text).empty());
}

TEST(doc, a_cell_can_name_a_picture) {
    // The one thing a cell could not say. An AssetRef is a Uuid, so a row that wanted an avatar
    // had nothing to write; the name the Assets panel shows is what it writes instead — the same
    // currency a script already spends on play_sound("click").
    Document doc;
    const Uuid ada = doc.AddAsset("avatar-ada", "art/ada.png");
    doc.AddAsset("avatar-grace", "art/grace.png");
    const Uuid fallback = doc.AddAsset("avatar-unknown", "art/unknown.png");

    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid list = doc.CreateNode(NodeKind::Frame, screen, "People");
    const Uuid row = doc.CreateNode(NodeKind::Frame, list, "Person");
    const Uuid picture = doc.CreateNode(NodeKind::Image, row, "Avatar");
    doc.SetProp(picture, Prop::Image, AssetRef{ fallback });
    doc.SetProp(picture, Prop::Field, std::string("avatar"));

    RowTable table;
    table.columns = { "avatar" };
    table.cells = { "avatar-ada", "", "nobody-has-this" };

    const auto flat = doc.Flatten(screen, [&](Uuid node, Uuid) -> const RowTable* {
        return node == list ? &table : nullptr;
    });

    std::vector<Value> images;
    for (const auto& node : flat)
        if (node.kind == NodeKind::Image) {
            const Value* value = node.props.Find(Prop::Image);
            images.push_back(value ? *value : Value{});
        }
    CHECK_EQ(images.size(), 3u);
    CHECK(images[0] == Value{ AssetRef{ ada } });
    // No picture in the row means no picture, not the template's placeholder left behind.
    CHECK(!IsSet(images[1]));
    // A name nothing answers to keeps what the designer drew, so a typo is a wrong picture rather
    // than a hole in the layout.
    CHECK(images[2] == Value{ AssetRef{ fallback } });
}

TEST(doc, an_empty_table_empties_its_container_and_nothing_else) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen);
    const Uuid list = doc.CreateNode(NodeKind::Frame, screen, "Messages");
    doc.SetProp(list, Prop::Repeat, 3.0f);
    doc.CreateNode(NodeKind::Frame, list, "Message");
    doc.CreateNode(NodeKind::Text, list, "Empty state");   // a sibling of the template, not a copy
    doc.CreateNode(NodeKind::Text, screen, "Composer");

    RowTable table;
    table.columns = { "body" };

    const auto flat = doc.Flatten(screen, [&](Uuid node, Uuid) -> const RowTable* {
        return node == list ? &table : nullptr;
    });

    // screen + list + the empty state + the composer; no copies at all.
    CHECK_EQ(flat.size(), 4u);
    for (const auto& node : flat) CHECK(!node.repeated);

    bool composer = false, empty = false;
    for (const auto& node : flat) {
        composer = composer || node.name == "Composer";
        empty = empty || node.name == "Empty state";
    }
    CHECK(composer);
    CHECK(empty);
}

TEST(doc, a_row_table_reads_cells_by_column_name) {
    RowTable table;
    table.columns = { "author", "body" };
    table.cells = { "Ada", "hello", "Grace", "hi" };

    CHECK_EQ(table.Count(), 2u);
    CHECK_EQ(table.ColumnOf("body"), 1);
    CHECK_EQ(table.ColumnOf("missing"), -1);
    CHECK_EQ(std::string(table.Cell(1, "author")), std::string("Grace"));
    CHECK(table.Cell(1, "missing").empty());
    CHECK(table.Cell(9, "author").empty());
}

TEST(doc, a_field_binding_survives_a_round_trip_through_the_serializer) {
    // The one thing that would break silently: a row template whose bindings are dropped on save
    // still looks right in the editor and draws blank the next time the project is opened.
    Document doc;
    const Uuid list = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Messages");
    doc.SetProp(list, Prop::Repeat, 3.0f);
    const Uuid body = doc.CreateNode(NodeKind::Text, list, "Body");
    doc.SetProp(body, Prop::Field, std::string("fill:tint"));

    const std::string xml = Serializer::ToXml(doc, true, nullptr, true);
    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.GetProp(list, Prop::Repeat), Value{ 3.0f });
    CHECK_EQ(loaded.GetProp(body, Prop::Field), Value{ std::string("fill:tint") });
}

// ------------------------------------------------------- format: what is not written

TEST(serializer, a_layout_at_its_defaults_is_not_written_at_all) {
    // Eleven of the nineteen layout fields are at their default on all but a percent or two of
    // real nodes. Writing them anyway was 40% of a document.
    Document doc;
    const Uuid bare = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Bare");
    const std::string xml = Serializer::ToXml(doc, true, nullptr, true);
    CHECK(xml.find("gap=") == std::string::npos);
    CHECK(xml.find("aspectRatio") == std::string::npos);
    CHECK(xml.find("minColumn") == std::string::npos);

    // ...and what does differ is still written, on its own.
    doc.Find(bare)->layout.gap = 12.0f;
    doc.Touch(bare);
    const std::string withGap = Serializer::ToXml(doc, true, nullptr, true);
    CHECK(withGap.find("gap=\"12\"") != std::string::npos);
    CHECK(withGap.find("minColumn") == std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(withGap, loaded, &error), error);
    CHECK_EQ(loaded.Find(bare)->layout.gap, 12.0f);
    CHECK_EQ(loaded.Find(bare)->layout.minColumn, layout::LayoutStyle{}.minColumn);
    CHECK(loaded.Find(bare)->layout.maxSize == layout::LayoutStyle{}.maxSize);
}

TEST(serializer, every_value_kind_round_trips_untagged) {
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Frame);
    doc.SetProp(node, Prop::Checked, true);
    doc.SetProp(node, Prop::FontSize, 17.5f);
    doc.SetProp(node, Prop::ShadowOffset, Vec2{ 3.0f, -4.0f });
    doc.SetProp(node, Prop::ShadowColor, Color{ 0.2f, 0.4f, 0.6f, 0.8f });
    doc.SetProp(node, Prop::Text, std::string("Send"));
    doc.SetProp(node, Prop::Fill, TokenRef{ "accent" });
    doc.SetProp(node, Prop::Value, Binding{ "user.name" });
    const Uuid asset = doc.AddAsset("logo", "art/logo.png");
    doc.SetProp(node, Prop::Image, AssetRef{ asset });
    doc.SetProp(node, Prop::Group, node);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(Serializer::ToXml(doc, true, nullptr, true), loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Checked), Value{ true });
    CHECK_EQ(loaded.GetProp(node, Prop::FontSize), Value{ 17.5f });
    CHECK((loaded.GetProp(node, Prop::ShadowOffset) == Value{ Vec2{ 3.0f, -4.0f } }));
    CHECK((loaded.GetProp(node, Prop::ShadowColor) == Value{ Color{ 0.2f, 0.4f, 0.6f, 0.8f } }));
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ std::string("Send") });
    CHECK(loaded.GetProp(node, Prop::Fill) == Value{ TokenRef{ "accent" } });
    CHECK(loaded.GetProp(node, Prop::Value) == Value{ Binding{ "user.name" } });
    CHECK(loaded.GetProp(node, Prop::Image) == Value{ AssetRef{ asset } });
    CHECK(loaded.GetProp(node, Prop::Group) == Value{ node });
}

TEST(serializer, numbers_are_written_as_themselves_not_as_widened_doubles) {
    // Every number in a document is an f32 and nlohmann's only float type is a double, so 0.6f
    // used to be written 0.6000000238418579 — nineteen characters that also fail to say what the
    // value is. The shortest f32 spelling narrows back to the same bits.
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Text);
    doc.SetProp(node, Prop::LetterSpacing, 0.6f);
    doc.SetProp(node, Prop::Opacity, 0.1f);
    doc.SetProp(node, Prop::ShadowColor, Color{ 0.29f, 0.427f, 0.808f, 1.0f });
    doc.Find(node)->layout.gap = 12.5f;
    doc.Find(node)->layout.aspectRatio = 1.0f / 3.0f;

    const std::string xml = Serializer::ToXml(doc, true, nullptr, true);
    CHECK(xml.find("0.6000000238418579") == std::string::npos);
    CHECK(xml.find("0.6") != std::string::npos);
    CHECK(xml.find("opacity=\"0.1\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::LetterSpacing), Value{ 0.6f });
    CHECK_EQ(loaded.GetProp(node, Prop::Opacity), Value{ 0.1f });
    CHECK((loaded.GetProp(node, Prop::ShadowColor) == Value{ Color{ 0.29f, 0.427f, 0.808f, 1.0f } }));
    CHECK_EQ(loaded.Find(node)->layout.gap, 12.5f);
    CHECK_EQ(loaded.Find(node)->layout.aspectRatio, 1.0f / 3.0f);
}

TEST(serializer, a_property_declares_what_it_holds) {
    // The table a text-only format reads with: `text="1"` is a string because Prop::Text says so,
    // and `fontSize="1"` is a number for the same reason. Unset is the honest answer for the ones
    // that are genuinely polymorphic, and is what puts them on the escape path instead.
    CHECK(PropValueType(Prop::Text) == ValueType::Text);
    CHECK(PropValueType(Prop::FontSize) == ValueType::Number);
    CHECK(PropValueType(Prop::Fill) == ValueType::Colour);
    CHECK(PropValueType(Prop::Enabled) == ValueType::Bool);
    CHECK(PropValueType(Prop::ShadowOffset) == ValueType::Vector2);
    CHECK(PropValueType(Prop::Image) == ValueType::Asset);
    CHECK(PropValueType(Prop::Resizable) == ValueType::Bool);
    CHECK(PropValueType(Prop::Value) == ValueType::Unset);       // text on a field, number on a slider
    CHECK(PropValueType(Prop::Series) == ValueType::Unset);
}

TEST(serializer, a_colour_that_does_not_fit_in_hex_keeps_its_floats) {
    // A colour from a picker is 8-bit and writes as "#rrggbbaa"; one computed in code is not, and
    // must not be quietly rounded on the way to disk.
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Frame);
    const Color exact{ 1.0f, 0.0f, 0.5019608f, 1.0f };     // #ff0080ff
    const Color computed{ 0.1234567f, 0.5f, 0.5f, 1.0f };
    doc.SetProp(node, Prop::Fill, exact);
    doc.SetProp(node, Prop::Stroke, computed);

    const std::string xml = Serializer::ToXml(doc, true, nullptr, true);
    CHECK(xml.find("#ff0080") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK(loaded.GetProp(node, Prop::Fill) == Value{ exact });
    CHECK(loaded.GetProp(node, Prop::Stroke) == Value{ computed });
}

TEST(serializer, a_string_that_starts_with_a_sigil_is_still_a_string) {
    // Untagged values mean "@x" is a token reference — so a label that really does read "@mkonic"
    // has to survive the trip, and so does a hex colour someone typed as text.
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Text);
    doc.SetProp(node, Prop::Text, std::string("@mkonic"));
    doc.SetProp(node, Prop::Placeholder, std::string("#general"));
    doc.SetProp(node, Prop::Tooltip, std::string("=SUM(A1:A9)"));
    doc.SetProp(node, "custom:cost", std::string("$40"));

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(Serializer::ToXml(doc, true, nullptr, true), loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ std::string("@mkonic") });
    CHECK_EQ(loaded.GetProp(node, Prop::Placeholder), Value{ std::string("#general") });
    CHECK_EQ(loaded.GetProp(node, Prop::Tooltip), Value{ std::string("=SUM(A1:A9)") });
    CHECK(*loaded.Find(node)->props.Find("custom:cost") == Value{ std::string("$40") });
}


// ------------------------------------------------------------------ format 3 (xml)

namespace {
    // A file writes an id only on a node something references — 6 of Vaecord's 540 — so a test that
    // looks a node up by id afterwards is asserting something the format deliberately stopped
    // promising. The tests below are about what survives a round trip, so they keep the ids; the id
    // policy itself is tested at the end of this section.
    std::string ToXmlKeepingIds(const Document& doc) {
        return Serializer::ToXml(doc, true, nullptr, true);
    }
}

TEST(xml, every_value_kind_round_trips_through_an_attribute) {
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Box");
    doc.SetProp(node, Prop::Checked, true);
    doc.SetProp(node, Prop::FontSize, 17.5f);
    doc.SetProp(node, Prop::ShadowOffset, Vec2{ 3.0f, -4.0f });
    doc.SetProp(node, Prop::ShadowColor, Color{ 0.2f, 0.4f, 0.6f, 0.8f });
    doc.SetProp(node, Prop::Text, std::string("Send"));
    doc.SetProp(node, Prop::Fill, TokenRef{ "accent" });
    doc.SetProp(node, Prop::Value, Binding{ "user.name" });
    const Uuid asset = doc.AddAsset("logo", "art/logo.png");
    doc.SetProp(node, Prop::Image, AssetRef{ asset });
    doc.SetProp(node, Prop::Group, node);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(ToXmlKeepingIds(doc), loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Checked), Value{ true });
    CHECK_EQ(loaded.GetProp(node, Prop::FontSize), Value{ 17.5f });
    CHECK((loaded.GetProp(node, Prop::ShadowOffset) == Value{ Vec2{ 3.0f, -4.0f } }));
    CHECK((loaded.GetProp(node, Prop::ShadowColor) == Value{ Color{ 0.2f, 0.4f, 0.6f, 0.8f } }));
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ std::string("Send") });
    CHECK(loaded.GetProp(node, Prop::Fill) == Value{ TokenRef{ "accent" } });
    CHECK(loaded.GetProp(node, Prop::Value) == Value{ Binding{ "user.name" } });
    CHECK(loaded.GetProp(node, Prop::Image) == Value{ AssetRef{ asset } });
    CHECK(loaded.GetProp(node, Prop::Group) == Value{ node });
}

TEST(xml, a_declared_type_is_what_tells_a_label_from_a_number) {
    // The one real ambiguity an attribute-only format introduces: every value is text, so `text="1"`
    // and `fontSize="1"` look identical. The property says which it is.
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Text);
    doc.SetProp(node, Prop::Text, std::string("1"));
    doc.SetProp(node, Prop::FontSize, 1.0f);
    doc.SetProp(node, Prop::Placeholder, std::string("12 8"));      // would read as a vector
    doc.SetProp(node, Prop::Tooltip, std::string("true"));          // would read as a bool

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("text=\"1\"") != std::string::npos);             // no escape needed
    CHECK(xml.find("fontSize=\"1\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ std::string("1") });
    CHECK_EQ(loaded.GetProp(node, Prop::FontSize), Value{ 1.0f });
    CHECK_EQ(loaded.GetProp(node, Prop::Placeholder), Value{ std::string("12 8") });
    CHECK_EQ(loaded.GetProp(node, Prop::Tooltip), Value{ std::string("true") });
}

TEST(xml, a_custom_property_has_no_declared_type_so_it_escapes_instead) {
    // Nobody declares what "hovered:badge" holds, so shape decides — and a string that would read
    // as something else takes the '$' escape, exactly as in format 2.
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Frame);
    doc.SetProp(node, "hovered:fill", TokenRef{ "accent" });
    doc.SetProp(node, "meta:count", std::string("7"));
    doc.SetProp(node, "meta:flag", std::string("false"));
    doc.SetProp(node, "meta:size", 12.0f);

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("hovered.fill=\"@accent\"") != std::string::npos);
    CHECK(xml.find("meta.count=\"$7\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    const PropBag& props = loaded.Find(node)->props;
    CHECK(*props.Find("hovered:fill") == Value{ TokenRef{ "accent" } });
    CHECK(*props.Find("meta:count") == Value{ std::string("7") });
    CHECK(*props.Find("meta:flag") == Value{ std::string("false") });
    CHECK(*props.Find("meta:size") == Value{ 12.0f });
}

TEST(xml, a_property_no_attribute_could_carry_goes_out_long_hand) {
    // A number stored on a property declared to hold text: the attribute would read back as a
    // string, so it takes the <prop> element, which says its own type. Pathological, and the point
    // is that it round-trips rather than being silently changed.
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Text);
    doc.SetProp(node, Prop::Text, 42.0f);

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("<prop name=\"text\" type=\"number\" value=\"42\"/>") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ 42.0f });
}

TEST(xml, a_colour_that_does_not_fit_in_hex_keeps_its_floats) {
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Frame);
    const Color exact{ 1.0f, 0.0f, 0.5019608f, 1.0f };
    const Color computed{ 0.1234567f, 0.5f, 0.5f, 1.0f };
    doc.SetProp(node, Prop::Fill, exact);
    doc.SetProp(node, Prop::Stroke, computed);

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("fill=\"#ff0080\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK(loaded.GetProp(node, Prop::Fill) == Value{ exact });
    CHECK(loaded.GetProp(node, Prop::Stroke) == Value{ computed });
}

TEST(xml, a_size_says_its_mode_in_its_spelling) {
    Document doc;
    const Uuid a = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "A");
    const Uuid b = doc.CreateNode(NodeKind::Frame, a, "B");
    const Uuid c = doc.CreateNode(NodeKind::Frame, a, "C");
    const Uuid d = doc.CreateNode(NodeKind::Frame, a, "D");
    doc.Find(a)->layout.width = layout::Size::Px(72.0f);
    doc.Find(b)->layout.width = layout::Size::Hug();
    doc.Find(b)->layout.height = layout::Size::Fill();
    doc.Find(c)->layout.width = layout::Size::Fill(2.0f);
    doc.Find(d)->layout.width = layout::Size::Percent(0.5f);

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("width=\"72\"") != std::string::npos);
    CHECK(xml.find("height=\"fill\"") != std::string::npos);
    CHECK(xml.find("width=\"fill 2\"") != std::string::npos);
    CHECK(xml.find("width=\"50%\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK(loaded.Find(a)->layout.width == layout::Size::Px(72.0f));
    CHECK(loaded.Find(b)->layout.width == layout::Size::Hug());
    CHECK(loaded.Find(b)->layout.height == layout::Size::Fill());
    CHECK(loaded.Find(c)->layout.width == layout::Size::Fill(2.0f));
    CHECK(loaded.Find(d)->layout.width == layout::Size::Percent(0.5f));
}

TEST(xml, padding_collapses_to_the_shortest_form_that_says_it) {
    Document doc;
    const Uuid all = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "All");
    const Uuid hv = doc.CreateNode(NodeKind::Frame, all, "HV");
    const Uuid four = doc.CreateNode(NodeKind::Frame, all, "Four");
    doc.Find(all)->layout.padding = Edges{ 12.0f };
    doc.Find(hv)->layout.padding = Edges{ 16.0f, 8.0f };
    doc.Find(four)->layout.padding = Edges{ 1.0f, 2.0f, 3.0f, 4.0f };

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("padding=\"12\"") != std::string::npos);
    CHECK(xml.find("padding=\"16 8\"") != std::string::npos);
    CHECK(xml.find("padding=\"1 2 3 4\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK(loaded.Find(all)->layout.padding == Edges{ 12.0f });
    CHECK((loaded.Find(hv)->layout.padding == Edges{ 16.0f, 8.0f }));
    CHECK((loaded.Find(four)->layout.padding == Edges{ 1.0f, 2.0f, 3.0f, 4.0f }));
}

TEST(xml, every_enum_is_a_name_rather_than_a_number) {
    // JSON was writing align, justify and the two constraints as raw u8, which is unreadable and
    // one enumerator insertion away from silently remapping every file that exists.
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Frame);
    layout::LayoutStyle& s = doc.Find(node)->layout;
    s.mode = layout::LayoutMode::Grid;
    s.axis = layout::Axis::Row;
    s.align = layout::Align::Stretch;
    s.justify = layout::Justify::SpaceBetween;
    s.constraintX = layout::Constraint::StartEnd;
    s.constraintY = layout::Constraint::Scale;

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("mode=\"grid\"") != std::string::npos);
    CHECK(xml.find("axis=\"row\"") != std::string::npos);
    CHECK(xml.find("align=\"stretch\"") != std::string::npos);
    CHECK(xml.find("justify=\"spaceBetween\"") != std::string::npos);
    CHECK(xml.find("constraintX=\"startEnd\"") != std::string::npos);
    CHECK(xml.find("constraintY=\"scale\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK(loaded.Find(node)->layout == s);
}

TEST(xml, a_label_with_a_newline_goes_in_the_element_body) {
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Text, Uuid::Invalid(), "Blurb");
    doc.SetProp(node, Prop::Text, std::string("line one\nline two"));

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("&#10;") == std::string::npos);
    CHECK(xml.find(">line one\nline two</text>") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ std::string("line one\nline two") });
}

TEST(xml, markup_characters_in_a_label_survive) {
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Text);
    doc.SetProp(node, Prop::Text, std::string("Tom & Jerry <3 \"quoted\""));
    doc.SetProp(node, Prop::Placeholder, std::string("a > b"));

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(ToXmlKeepingIds(doc), loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ std::string("Tom & Jerry <3 \"quoted\"") });
    CHECK_EQ(loaded.GetProp(node, Prop::Placeholder), Value{ std::string("a > b") });
}

TEST(xml, a_string_that_starts_with_a_sigil_is_still_a_string) {
    Document doc;
    const Uuid node = doc.CreateNode(NodeKind::Text);
    doc.SetProp(node, Prop::Text, std::string("@mkonic"));
    doc.SetProp(node, Prop::Placeholder, std::string("#general"));
    doc.SetProp(node, Prop::Tooltip, std::string("=SUM(A1:A9)"));
    doc.SetProp(node, Prop::Group, std::string("&anded"));
    doc.SetProp(node, Prop::Route, std::string("*starred"));
    doc.SetProp(node, "custom:cost", std::string("$40"));

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(ToXmlKeepingIds(doc), loaded, &error), error);
    CHECK_EQ(loaded.GetProp(node, Prop::Text), Value{ std::string("@mkonic") });
    CHECK_EQ(loaded.GetProp(node, Prop::Placeholder), Value{ std::string("#general") });
    CHECK_EQ(loaded.GetProp(node, Prop::Tooltip), Value{ std::string("=SUM(A1:A9)") });
    CHECK_EQ(loaded.GetProp(node, Prop::Group), Value{ std::string("&anded") });
    CHECK_EQ(loaded.GetProp(node, Prop::Route), Value{ std::string("*starred") });
    CHECK(*loaded.Find(node)->props.Find("custom:cost") == Value{ std::string("$40") });
}

TEST(xml, the_tree_is_the_nesting_and_flags_are_written_only_when_set) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid frame = doc.CreateNode(NodeKind::Frame, screen, "Card");
    const Uuid label = doc.CreateNode(NodeKind::Text, frame, "Title");
    doc.Find(frame)->visible = false;
    doc.Find(frame)->slot = true;
    doc.Find(label)->locked = true;
    doc.SetStartScreen(screen);

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("hidden=\"true\"") != std::string::npos);
    CHECK(xml.find("visible=") == std::string::npos);          // the node flag is spelled 'hidden'
    CHECK(xml.find("parent=") == std::string::npos);
    CHECK(xml.find("children=") == std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.Find(screen)->children.size(), 1u);
    CHECK_EQ(loaded.Find(frame)->children.size(), 1u);
    CHECK_EQ(loaded.Find(frame)->parent, screen);
    CHECK_EQ(loaded.Find(label)->parent, frame);
    CHECK(!loaded.Find(frame)->visible);
    CHECK(loaded.Find(frame)->slot);
    CHECK(loaded.Find(label)->locked);
    CHECK(loaded.Find(screen)->visible);
    CHECK_EQ(loaded.StartScreen(), screen);
}

TEST(xml, tokens_and_assets_travel_with_the_document) {
    Document doc;
    doc.CreateNode(NodeKind::Screen);
    doc.SetToken("accent", Token{ Color{ 0.1f, 0.2f, 0.3f, 1.0f }, Color{ 0.1f, 0.2f, 0.3f, 1.0f } });
    doc.SetToken("bg", Token{ Color{ 1, 1, 1, 1 }, Color{ 0, 0, 0, 1 }, "page background" });
    const Uuid asset = doc.AddAsset("logo", "art/logo.png");

    const std::string xml = ToXmlKeepingIds(doc);
    // A token whose two themes agree says its value once.
    CHECK(xml.find("<token name=\"accent\" value=") != std::string::npos);
    CHECK(xml.find("desc=\"page background\"") != std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK(loaded.Tokens() == doc.Tokens());
    CHECK_EQ(loaded.Assets().size(), 1u);
    CHECK_EQ(loaded.Assets()[0].id, asset);
    CHECK_EQ(loaded.Assets()[0].path, std::string("art/logo.png"));
}

TEST(xml, sample_rows_get_an_element_rather_than_an_escaped_attribute) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid list = doc.CreateNode(NodeKind::Frame, screen, "Messages");
    doc.SetProp(list, Prop::Repeat, 3.0f);
    doc.SetProp(list, Prop::Sample, std::string("author | body\nAda | Hello\nGrace | Hi\n"));
    doc.CreateNode(NodeKind::Frame, list, "Message");

    const std::string xml = ToXmlKeepingIds(doc);
    CHECK(xml.find("<sample>author | body") != std::string::npos);
    // The whole point of the element: no &#10; anywhere near it.
    CHECK(xml.find("&#10;") == std::string::npos);

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.Find(list)->props.Text(Prop::Sample), doc.Find(list)->props.Text(Prop::Sample));
    CHECK_EQ(ParseRowText(loaded.Find(list)->props.Text(Prop::Sample)).Count(), 2u);
}

TEST(xml, an_override_keeps_the_id_it_keys_on) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid master = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Card");
    const Uuid inner = doc.CreateNode(NodeKind::Text, master, "Label");
    const Uuid component = doc.MakeComponent(master, "Card");
    const Uuid instance = doc.CreateInstance(component, screen);
    doc.SetOverride(instance, inner, Prop::Text, std::string("Hello"));
    doc.SetOverride(instance, inner, "hovered:fill", TokenRef{ "accent" });

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(ToXmlKeepingIds(doc), loaded, &error), error);
    const Node* back = loaded.Find(instance);
    CHECK(back != nullptr);
    CHECK_EQ(back->componentId, component);
    CHECK_EQ(back->overrides.size(), 1u);
    CHECK(back->overrides.contains(inner));
    CHECK(*back->overrides.at(inner).Find(Prop::Text) == Value{ std::string("Hello") });
    CHECK(*back->overrides.at(inner).Find("hovered:fill") == Value{ TokenRef{ "accent" } });
}

TEST(xml, a_malformed_document_says_which_line_and_loads_nothing) {
    Document loaded;
    loaded.CreateNode(NodeKind::Screen);
    std::string error;
    const std::string broken =
        "<vae version=\"4\" theme=\"dark\">\n"
        "  <screen name=\"Home\">\n"
        "    <frame name=\"Unclosed\">\n"
        "  </screen>\n"
        "</vae>\n";
    CHECK(!Serializer::FromXml(broken, loaded, &error));
    CHECK(error.find("line ") != std::string::npos);

    // An element the format does not have is refused rather than dropped: silently losing a node
    // is worse than not opening the file.
    std::string error2;
    Document other;
    CHECK(!Serializer::FromXml("<vae version=\"4\"><widget name=\"x\"/></vae>", other, &error2));
    CHECK(error2.find("widget") != std::string::npos);

    // The same for an attribute nobody declared.
    std::string error3;
    Document third;
    CHECK(!Serializer::FromXml("<vae version=\"4\"><frame witdh=\"12\"/></vae>", third, &error3));
    CHECK(error3.find("witdh") != std::string::npos);
}

TEST(xml, a_document_from_any_other_format_is_refused_rather_than_half_read) {
    // One format, so this refuses in both directions: a file from a build that has not happened yet
    // and a file from one that no longer exists are equally unreadable, and saying which format the
    // file is beats guessing at it.
    Document loaded;
    std::string error;
    CHECK(!Serializer::FromXml("<vae version=\"99\"><screen name=\"Home\"/></vae>", loaded, &error));
    CHECK(error.find("99") != std::string::npos);
    CHECK(error.find(std::to_string(Serializer::kFormatVersion)) != std::string::npos);

    CHECK(!Serializer::FromXml("<vae version=\"3\"><screen name=\"Home\"/></vae>", loaded, &error));
    CHECK(error.find("format 3") != std::string::npos);
    CHECK_EQ(loaded.NodeCount(), 0u);
}




TEST(xml, an_id_is_written_only_when_something_refers_to_it) {
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid plain = doc.CreateNode(NodeKind::Frame, screen, "Card");
    doc.CreateNode(NodeKind::Text, plain, "Title");
    const Uuid master = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Chip");
    const Uuid inner = doc.CreateNode(NodeKind::Text, master, "Label");
    const Uuid component = doc.MakeComponent(master, "Chip");
    const Uuid instance = doc.CreateInstance(component, screen);
    doc.SetOverride(instance, inner, Prop::Text, std::string("Hello"));
    doc.SetStartScreen(screen);

    const std::string xml = Serializer::ToXml(doc);
    // Referenced: the start screen, the component an instance points at, and the node an override
    // keys on — plus everything inside a component master, which is what overrides key on.
    CHECK(xml.find(screen.ToString()) != std::string::npos);
    CHECK(xml.find(component.ToString()) != std::string::npos);
    CHECK(xml.find(inner.ToString()) != std::string::npos);
    // Not referenced: a plain frame, its label, and the instance itself.
    CHECK(xml.find(plain.ToString()) == std::string::npos);
    CHECK(xml.find(instance.ToString()) == std::string::npos);

    // And the document still comes back whole — the tree is the nesting, not the ids.
    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(xml, loaded, &error), error);
    CHECK_EQ(loaded.NodeCount(), doc.NodeCount());
    const Node* back = loaded.Find(screen);
    CHECK(back != nullptr);
    CHECK_EQ(back->children.size(), 2u);
    CHECK_EQ(loaded.Find(back->children[0])->name, std::string("Card"));
    CHECK_EQ(loaded.Find(loaded.Find(back->children[0])->children[0])->name, std::string("Title"));
    // The instance got a fresh id, and still points at the same component and keys on the same node.
    const Node* copy = loaded.Find(back->children[1]);
    CHECK(copy != nullptr);
    CHECK_EQ(copy->componentId, component);
    CHECK(copy->overrides.contains(inner));
}

TEST(xml, keeping_ids_is_what_an_in_memory_snapshot_asks_for) {
    // The Play/Stop snapshot restores the document in place so observers keep their subscriptions,
    // and an observer that survives that is holding an id. A file drops them; this cannot.
    Document doc;
    const Uuid screen = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid card = doc.CreateNode(NodeKind::Frame, screen, "Card");
    const Uuid label = doc.CreateNode(NodeKind::Text, card, "Title");

    Document loaded;
    std::string error;
    CHECK_MESSAGE(Serializer::FromXml(ToXmlKeepingIds(doc), loaded, &error), error);
    CHECK(loaded.Find(screen) != nullptr);
    CHECK(loaded.Find(card) != nullptr);
    CHECK(loaded.Find(label) != nullptr);
    CHECK_EQ(loaded.Find(label)->parent, card);
}


// ------------------------------------------------------------------ a project split across files

namespace {

    // A project with two screens, a forked component used by both, a token and an asset — enough
    // that splitting it has something to get wrong in every direction.
    Document BuildSplittable(Uuid& outCard, Uuid& outHome) {
        Document doc;
        ui::StandardLibrary().Install("vae.std", 0, doc);

        doc.SetToken("brand", Token{ Value(Color{ 0.1f, 0.2f, 0.3f, 1.0f }),
                                     Value(Color{ 0.3f, 0.4f, 0.5f, 1.0f }), "the one colour" });
        doc.AddAsset("logo", "assets/logo.svg", Uuid::Invalid());

        const Uuid card = doc.CreateNode(NodeKind::Frame, Uuid::Invalid(), "Card");
        const Uuid label = doc.CreateNode(NodeKind::Text, card, "Label");
        doc.SetProp(label, Prop::Text, std::string("Hello"));
        doc.MakeComponent(card, "Card");
        outCard = card;

        const Uuid home = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Home");
        doc.CreateInstance(card, home);
        doc.SetStartScreen(home);
        outHome = home;

        const Uuid settings = doc.CreateNode(NodeKind::Screen, Uuid::Invalid(), "Settings");
        doc.CreateInstance(card, settings);
        return doc;
    }

    std::filesystem::path ScratchProject(const char* name) {
        const auto dir = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir / Project::kFileName;
    }

}

TEST(project, a_split_project_is_one_file_per_screen) {
    Uuid card, home;
    const Document doc = BuildSplittable(card, home);

    Project project;
    project.name = "Split";
    const auto file = ScratchProject("vae-split-one");
    CHECK(Project::SaveDocument(doc, project, file, &ui::StandardLibrary()));

    // One file per screen, named after the screen — the whole point, because a diff has to say
    // which screen changed. The folder is the answer to "which screens are there": the index does
    // not carry a second copy of it that could disagree.
    CHECK_EQ(Project::DocumentsIn(file.parent_path()).size(), 3u);
    CHECK(std::filesystem::exists(file.parent_path() / "screens/Home.vae"));
    CHECK(std::filesystem::exists(file.parent_path() / "screens/Settings.vae"));

    // The forked component gets its own file; the fifty the catalog builds do not. And components
    // are read first, so they come first.
    CHECK(std::filesystem::exists(file.parent_path() / "components/Card.vae"));
    CHECK_EQ(Project::DocumentsIn(file.parent_path()).front().filename(),
             std::filesystem::path("Card.vae"));
    CHECK(std::filesystem::exists(file.parent_path() / "tokens.vae"));

    // A screen file holds its screen and not the others.
    const auto homeText = FileSystem::ReadText(file.parent_path() / "screens/Home.vae");
    CHECK(homeText.has_value());
    CHECK(homeText->find("Home") != std::string::npos);
    CHECK(homeText->find("Settings") == std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
}

TEST(project, a_split_project_reads_back_as_the_document_it_was) {
    Uuid card, home;
    const Document doc = BuildSplittable(card, home);

    Project project;
    project.name = "Split";
    const auto file = ScratchProject("vae-split-two");
    CHECK(Project::SaveDocument(doc, project, file, &ui::StandardLibrary()));

    Document back;
    Project loadedProject;
    std::string error;
    CHECK(Project::LoadDocument(file, back, loadedProject, &error, &ui::StandardLibrary()));
    CHECK(error.empty());

    // The same screens, the same start screen, the same token, the same asset.
    std::vector<std::string> screens;
    for (Uuid id : back.Roots())
        if (const Node* node = back.Find(id); node && node->kind == NodeKind::Screen)
            screens.push_back(node->name);
    std::sort(screens.begin(), screens.end());
    CHECK_EQ(screens.size(), 2u);
    CHECK_EQ(screens[0], std::string("Home"));
    CHECK_EQ(screens[1], std::string("Settings"));

    const Node* start = back.Find(back.StartScreen());
    CHECK(start != nullptr);
    if (start) CHECK_EQ(start->name, std::string("Home"));

    CHECK(back.Tokens().contains("brand"));
    CHECK_EQ(back.Assets().size(), std::size_t(1));

    // And the instances on both screens still point at the component the other file defines —
    // which is the one thing splitting a document could quietly break.
    const Node* forked = back.Find(card);
    CHECK(forked != nullptr);
    u32 instances = 0;
    for (Uuid id : back.AllNodes())
        if (const Node* node = back.Find(id); node && node->IsInstance() && node->componentId == card)
            ++instances;
    CHECK_EQ(instances, 2u);

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
}

TEST(project, a_deleted_screen_stops_being_on_disk) {
    Uuid card, home;
    Document doc = BuildSplittable(card, home);

    Project project;
    project.name = "Split";
    const auto file = ScratchProject("vae-split-three");
    CHECK(Project::SaveDocument(doc, project, file, &ui::StandardLibrary()));
    CHECK(std::filesystem::exists(file.parent_path() / "screens/Settings.vae"));

    for (Uuid id : doc.Roots())
        if (const Node* node = doc.Find(id); node && node->name == "Settings") { doc.DeleteNode(id); break; }

    // Left behind, the file would still load, because the folder is what the loader reads: a
    // deleted screen would come back on the next open.
    CHECK(Project::SaveDocument(doc, project, file, &ui::StandardLibrary()));
    CHECK(!std::filesystem::exists(file.parent_path() / "screens/Settings.vae"));
    CHECK_EQ(Project::DocumentsIn(file.parent_path()).size(), 2u);

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
}

