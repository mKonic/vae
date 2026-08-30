// Direct manipulation: what the mouse does to a document.
#include "Harness.h"

namespace vae::selftest {

    // ---------------------------------------------------------------------------- the checks

    void TestHitTest() {
        Section("hit test");
        Driver driver;
        EditorState& state = driver.State();

        Check(driver.Surface().HasViewport(), "canvas has a viewport after two frames");

        const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
        const Uuid b = PlaceFixed(state, "Button", { 500.0f, 300.0f }, { 200.0f, 48.0f });
        Check(a.Valid() && b.Valid(), "two buttons placed");

        state.ClearSelection();
        driver.ClickDoc({ 200.0f, 124.0f });
        Check(state.Selection().size() == 1 && state.Primary() == a,
              "a click inside a widget selects it");

        // The click landed on the button's label, which is a node inside the component. Picking
        // that instead of the instance is the bug this guards: you would be editing every
        // button in the project at once.
        const doc::Node* picked = state.Doc().Find(state.Primary());
        Check(picked && picked->IsInstance(), "the pick is the instance, not the component's internals");

        driver.ClickDoc({ 900.0f, 600.0f });
        Check(state.Selection().empty(), "a click on empty canvas clears the selection");

        driver.ClickDoc({ 600.0f, 324.0f });
        Check(state.Primary() == b, "the second widget is picked at its own coordinates");
    }


    void TestDrag() {
        Section("drag");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
        state.ClearSelection();
        driver.ClickDoc({ 200.0f, 124.0f });

        const std::size_t before = state.Commands().UndoDepth();
        driver.DragDoc({ 200.0f, 124.0f }, { 260.0f, 204.0f });

        const Vec2 moved = OffsetOf(state, a);
        Check(Near(moved.x, 160.0f) && Near(moved.y, 180.0f),
              "the widget moved by the drag delta");

        // Eight move frames, eight SetLayout commands, one undo entry. Without coalescing a
        // single drag would take eight presses of Ctrl+Z to undo.
        Check(state.Commands().UndoDepth() == before + 1,
              "the whole drag is one undo entry");

        state.Undo();
        const Vec2 restored = OffsetOf(state, a);
        Check(Near(restored.x, 100.0f) && Near(restored.y, 100.0f),
              "undo puts it back where it started");

        state.Redo();
        const Vec2 again = OffsetOf(state, a);
        Check(Near(again.x, 160.0f) && Near(again.y, 180.0f), "redo moves it again");
    }


    void TestSnap() {
        Section("snap");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
        PlaceFixed(state, "Button", { 500.0f, 300.0f }, { 200.0f, 48.0f });

        state.ClearSelection();
        driver.ClickDoc({ 200.0f, 124.0f });

        // Three units short of the sibling's left edge: inside the snap threshold, so the drag
        // should finish exactly aligned rather than three pixels off.
        driver.DragDoc({ 200.0f, 124.0f }, { 597.0f, 124.0f });

        const Vec2 snapped = OffsetOf(state, a);
        Check(Near(snapped.x, 500.0f, 0.01f), "the left edge snapped to the sibling's left edge");
        Check(Near(snapped.y, 100.0f, 0.01f), "the axis with nothing to snap to did not move");
        Check(!driver.GuidesDuringDrag().empty(), "a guide was published while snapped");

        // Well clear of every candidate line, so the drag lands exactly where it was dropped.
        state.ClearSelection();
        driver.ClickDoc({ 600.0f, 124.0f });
        driver.DragDoc({ 600.0f, 124.0f }, { 663.0f, 191.0f });
        const Vec2 free = OffsetOf(state, a);
        Check(Near(free.x, 563.0f) && Near(free.y, 167.0f), "a drag far from any line is not snapped");
    }


    void TestResize() {
        Section("resize");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
        state.ClearSelection();
        driver.ClickDoc({ 200.0f, 124.0f });

        // The bottom-right handle sits on the corner itself while the shape is large enough to
        // host it; drag it out by 60x20.
        driver.DragDoc({ 300.0f, 148.0f }, { 360.0f, 168.0f });

        const doc::Node* node = state.Doc().Find(a);
        Check(node != nullptr, "the node survived the resize");
        if (node) {
            Check(node->layout.width.mode == layout::SizeMode::Fixed
               && node->layout.height.mode == layout::SizeMode::Fixed,
                  "a handle drag writes explicit sizes, never Hug");
            Check(Near(node->layout.width.value, 260.0f) && Near(node->layout.height.value, 68.0f),
                  "the new size matches the handle's travel");
            Check(Near(node->layout.offsetStart.x, 100.0f) && Near(node->layout.offsetStart.y, 100.0f),
                  "the opposite corner stayed put");
        }

        state.Undo();
        const doc::Node* undone = state.Doc().Find(a);
        Check(undone && Near(undone->layout.width.value, 200.0f), "undo restores the old size");
    }


    void TestMarquee() {
        Section("marquee");
        Driver driver;
        EditorState& state = driver.State();

        PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
        PlaceFixed(state, "Button", { 500.0f, 300.0f }, { 200.0f, 48.0f });
        state.ClearSelection();
        driver.Frame();

        driver.DragDoc({ 60.0f, 60.0f }, { 760.0f, 420.0f });
        Check(state.Selection().size() == 2, "a rubber band takes everything it touches");
    }


    void TestDroppingIntoAContainer() {
        Section("dropping into a container");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid card = PlaceFixed(state, "Card", { 120.0f, 120.0f }, { 320.0f, 220.0f });
        Check(card.Valid(), "a Card was placed");
        if (!card.Valid()) return;
        driver.Frame();

        const Rect box = driver.Surface().BoundsOf(state, card);
        // A container is a thing you put something in. Dropping onto one and getting a button
        // lying on top of it is the behaviour that makes the whole catalog of cards useless.
        const Uuid target = driver.Surface().DropTargetAt(state, box.Center());
        Check(target == card, "a drop on the card targets the card, not the screen");

        const Uuid outside = driver.Surface().DropTargetAt(state, { 900.0f, 600.0f });
        Check(outside == state.ActiveScreen(), "a drop on empty canvas still targets the screen");

        const Uuid button = state.PlaceInstance("Button", target, { 0.0f, 0.0f });
        state.EndGesture();
        driver.Frame();
        const doc::Node* placed = state.Doc().Find(button);
        Check(placed && placed->parent == card, "the button became a child of the card");

        const Rect inner = driver.Surface().BoundsOf(state, button);
        Check(inner.size.x > 0.0f && box.Contains(inner.Center()),
              "and it is drawn inside the card");
    }


    void TestFillingWidgetMoves() {
        Section("moving a filling widget");
        Driver driver;
        EditorState& state = driver.State();

        // Tabs is authored to fill its parent's width, which on an absolute screen means "the
        // rest of the screen from here". Dragging one used to move its start and let the solver
        // put the right edge straight back — a resize you could not escape without resizing.
        const Uuid tabs = state.PlaceInstance("Tabs", state.ActiveScreen(), { 200.0f, 200.0f });
        state.EndGesture();
        driver.Frame();
        Check(tabs.Valid(), "a Tabs was placed");
        if (!tabs.Valid()) return;

        const doc::Node* node = state.Doc().Find(tabs);
        Check(node && node->layout.width.mode == layout::SizeMode::Fill,
              "it starts out filling the width");

        state.Select(tabs);
        driver.Frame();
        const Rect before = driver.Surface().BoundsOf(state, tabs);
        Check(before.size.x > 0.0f, "it has a box on the canvas");

        driver.DragDoc(before.Center(), before.Center() + Vec2{ -120.0f, 40.0f });

        const doc::Node* after = state.Doc().Find(tabs);
        Check(after && after->layout.width.mode == layout::SizeMode::Fixed,
              "moving it gave it a width of its own");
        const Rect box = driver.Surface().BoundsOf(state, tabs);
        Check(Near(box.size.x, before.size.x, 1.0f),
              "and it is the same width it looked before the drag");
        Check(Near(box.pos.x, before.pos.x - 120.0f, 1.0f)
              && Near(box.pos.y, before.pos.y + 40.0f, 1.0f),
              "the drag moved it rather than resizing it");
    }


    void TestPlacementUndoes() {
        Section("undoing a placement");
        Driver driver;
        EditorState& state = driver.State();

        const std::size_t before = state.Commands().UndoDepth();
        const Uuid button = state.PlaceInstance("Button", state.ActiveScreen(),
                                                { 120.0f, 90.0f });
        driver.Frame();
        Check(button.Valid() && state.Doc().Contains(button), "a button was placed");
        Check(state.Commands().UndoDepth() == before + 1,
              "placing it is one undo entry, not two");

        // Placing a widget is the most-used gesture there is, and it was the one edit undo did
        // not cover: the position rolled back and the widget stayed.
        state.Undo();
        driver.Frame();
        Check(!state.Doc().Contains(button), "undo takes the widget with it");

        state.Redo();
        driver.Frame();
        Check(state.Doc().Contains(button), "redo brings it back");
        const doc::Node* again = state.Doc().Find(button);
        Check(again && Near(again->layout.offsetStart.x, 120.0f),
              "with the position it was dropped at");
        Check(again && again->IsInstance(), "and still as an instance of its component");
    }


    void TestDeleteAndDuplicate() {
        Section("delete and duplicate");
        Driver driver;
        EditorState& state = driver.State();

        const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
        state.Select(a);
        state.DuplicateSelection();
        Check(state.Selection().size() == 1 && state.Primary() != a, "duplicate selects the copy");
        const Uuid copy = state.Primary();
        const Vec2 offset = OffsetOf(state, copy);
        Check(Near(offset.x, 116.0f) && Near(offset.y, 116.0f), "the copy is nudged off the original");

        state.DeleteSelection();
        Check(state.Doc().Find(copy) == nullptr, "delete removes the node");
        Check(state.Selection().empty(), "and the selection with it");
        state.Undo();
        Check(state.Doc().Find(copy) != nullptr, "undo brings it back with the same id");

        // A screen root is not the canvas's to delete, even when it is selected.
        state.Select(state.ActiveScreen());
        state.DeleteSelection();
        Check(state.Doc().Find(state.ActiveScreen()) != nullptr, "a screen is not deleted by Del");
    }


    void TestAlignAndDistribute() {
        Section("align and distribute");
        Driver driver;
        EditorState& state = driver.State();
        Canvas& canvas = driver.Surface();

        const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 40.0f });
        const Uuid b = PlaceFixed(state, "Button", { 340.0f, 260.0f }, { 120.0f, 40.0f });
        const Uuid c = PlaceFixed(state, "Button", { 700.0f, 500.0f }, {  80.0f, 40.0f });
        driver.Frame();

        // Equal gaps between the outer two: 680 units of span, 400 of content, so 140 each.
        state.SelectMany({ a, b, c });
        canvas.DistributeSelection(state, true);
        driver.Frame();
        Check(Near(OffsetOf(state, a).x, 100.0f), "the leftmost stays put when distributing");
        Check(Near(OffsetOf(state, b).x, 440.0f), "the middle one takes the even gap");
        Check(Near(OffsetOf(state, c).x, 700.0f), "the rightmost stays put");

        canvas.AlignSelection(state, Canvas::Edge::Left);
        driver.Frame();
        Check(Near(OffsetOf(state, a).x, 100.0f) && Near(OffsetOf(state, b).x, 100.0f)
           && Near(OffsetOf(state, c).x, 100.0f), "align left brings them to the same edge");

        canvas.AlignSelection(state, Canvas::Edge::CentreY);
        driver.Frame();
        const f32 centre = OffsetOf(state, a).y + 20.0f;
        Check(Near(OffsetOf(state, b).y + 20.0f, centre)
           && Near(OffsetOf(state, c).y + 20.0f, centre), "centring is about the middles, not the tops");
        Check(Near(centre, 320.0f), "the selection's own bounding box is the reference");

        // One undo per command, not one per node moved.
        const std::size_t depth = state.Commands().UndoDepth();
        state.Undo();
        Check(state.Commands().UndoDepth() == depth - 1, "an align is a single undo entry");
        driver.Frame();
        Check(!Near(OffsetOf(state, b).y + 20.0f, centre), "and it really undoes");

        // A lone selection aligns against the screen instead, which is the only reference left.
        state.Select(a);
        canvas.AlignSelection(state, Canvas::Edge::Right);
        driver.Frame();
        Check(Near(OffsetOf(state, a).x, 1080.0f), "one selected widget aligns to the screen");

        canvas.DistributeSelection(state, true);
        Check(Near(OffsetOf(state, a).x, 1080.0f), "distributing fewer than three does nothing");
    }


    // Wrapping a selection in a frame, and dissolving one. The whole risk is arithmetic: a
    // group that lands somewhere else, or children that jump when they move into it.
    void TestGrouping() {
        Section("group and ungroup");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        doc::Document& d = state.Doc();
        Canvas& canvas = layer.Surface();

        // An absolute screen, which is where grouping has arithmetic to get wrong. (Inside a
        // stack the group hugs and the stack does the positioning; that path is checked at the
        // end.)
        {
            doc::Node* screen = d.Find(state.ActiveScreen());
            screen->layout.mode = layout::LayoutMode::Absolute;
            d.Touch(state.ActiveScreen());
        }

        // Two boxes on an absolute screen, 100 apart.
        const auto box = [&](const char* name, Vec2 at) {
            const Uuid id = d.CreateNode(doc::NodeKind::Frame, state.ActiveScreen(), name);
            doc::Node* node = d.Find(id);
            node->layout.mode = layout::LayoutMode::Absolute;
            node->layout.offsetStart = at;
            node->layout.width = layout::Size::Px(80.0f);
            node->layout.height = layout::Size::Px(40.0f);
            d.Touch(id);
            return id;
        };
        const Uuid left = box("Left box", { 100.0f, 100.0f });
        const Uuid right = box("Right box", { 200.0f, 160.0f });
        driver.Frame();

        const Rect leftBefore = canvas.BoundsOf(state, left);
        const Rect rightBefore = canvas.BoundsOf(state, right);

        state.SelectMany({ left, right });
        Check(canvas.CanGroup(state), "two siblings can be grouped");
        canvas.GroupSelection(state);
        driver.Frame();

        const Uuid group = state.Selection().empty() ? Uuid::Invalid() : state.Selection().front();
        if (!Check(group.Valid() && d.Find(group), "grouping selects the new frame")) return;
        Check(d.Find(left)->parent == group && d.Find(right)->parent == group,
              "both boxes are inside it");

        // The only thing that matters: nothing moved on screen.
        Check(Near(canvas.BoundsOf(state, left).pos.x, leftBefore.pos.x, 1.0f)
              && Near(canvas.BoundsOf(state, left).pos.y, leftBefore.pos.y, 1.0f),
              "the left box did not move");
        Check(Near(canvas.BoundsOf(state, right).pos.x, rightBefore.pos.x, 1.0f)
              && Near(canvas.BoundsOf(state, right).pos.y, rightBefore.pos.y, 1.0f),
              "and neither did the right one");

        const Rect groupBox = canvas.BoundsOf(state, group);
        Check(Near(groupBox.size.x, 180.0f, 1.0f) && Near(groupBox.size.y, 100.0f, 1.0f),
              "the group is the box the selection occupied: got "
              + std::to_string(groupBox.size.x) + "x" + std::to_string(groupBox.size.y)
              + " at " + std::to_string(groupBox.pos.x) + "," + std::to_string(groupBox.pos.y));

        // Undo puts them back where they were, still as siblings.
        state.Undo();
        driver.Frame();
        Check(d.Find(left)->parent == state.ActiveScreen(), "undo takes them back out");
        Check(Near(canvas.BoundsOf(state, left).pos.x, leftBefore.pos.x, 1.0f),
              "and leaves them where they were");

        state.Redo();
        driver.Frame();
        const Uuid regrouped = d.Find(left)->parent;
        Check(regrouped != state.ActiveScreen(), "redo groups them again");

        // And dissolving it leaves both boxes exactly where they are drawn.
        state.Select(regrouped);
        Check(canvas.CanUngroup(state), "a group can be ungrouped");
        canvas.UngroupSelection(state);
        driver.Frame();
        Check(d.Find(left)->parent == state.ActiveScreen(), "the boxes are siblings again");
        Check(d.Find(regrouped) == nullptr, "and the frame is gone");
        Check(Near(canvas.BoundsOf(state, left).pos.x, leftBefore.pos.x, 1.0f)
              && Near(canvas.BoundsOf(state, right).pos.y, rightBefore.pos.y, 1.0f),
              "with nothing having moved");

        // And inside a stack, a group is a stack that hugs: position is the parent's business
        // there, and a group with a hardcoded box would fight it.
        {
            doc::Node* screen = d.Find(state.ActiveScreen());
            screen->layout.mode = layout::LayoutMode::Stack;
            screen->layout.axis = layout::Axis::Row;
            screen->layout.gap = 0.0f;
            d.Touch(state.ActiveScreen());
        }
        driver.Frame();
        state.SelectMany({ left, right });
        canvas.GroupSelection(state);
        driver.Frame();
        const Uuid stackGroup = state.Selection().front();
        const doc::Node* stacked = d.Find(stackGroup);
        Check(stacked && stacked->layout.mode == layout::LayoutMode::Stack,
              "a group inside a stack is a stack");
        Check(stacked && stacked->layout.width.mode == layout::SizeMode::Hug,
              "and it hugs rather than pinning a box");
        Check(Near(canvas.BoundsOf(state, stackGroup).size.x, 160.0f, 2.0f),
              "so it comes out exactly as wide as the two boxes in it");
    }


    void TestNesting() {
        Section("nesting");
        Driver driver;
        EditorState& state = driver.State();

        // A Card made of a Button: components made of components, which is what a catalog is.
        // Built detached, or sealing it would leave the master sitting on the screen.
        const Uuid cardRoot = state.Doc().CreateNode(doc::NodeKind::Frame, Uuid::Invalid(),
                                                     "Card");
        {
            doc::Node* node = state.Doc().Find(cardRoot);
            node->layout.mode = layout::LayoutMode::Stack;
            node->layout.axis = layout::Axis::Column;
            node->layout.width = layout::Size::Px(200.0f);
            node->layout.height = layout::Size::Px(80.0f);
        }
        state.Doc().CreateInstance(state.Library().Find("Button"), cardRoot);
        const Uuid card = state.Doc().MakeComponent(cardRoot, "Card");
        Check(card.Valid(), "a component sealed from a frame holding a widget");

        const Uuid first  = state.Doc().CreateInstance(card, state.ActiveScreen());
        const Uuid second = state.Doc().CreateInstance(card, state.ActiveScreen());
        state.Doc().Find(second)->layout.offsetStart = { 0.0f, 200.0f };
        state.Doc().Touch(second);
        driver.Frame();

        // The Button inside the Card is authored once, so both copies show the same node — and
        // that node is only addressable through the copy it is being edited in.
        const doc::Node* cardNode = state.Doc().Find(card);
        Check(cardNode && !cardNode->children.empty(), "the card has the button inside it");
        if (!cardNode || cardNode->children.empty()) return;
        const Uuid inner = cardNode->children.front();

        state.SelectInside({ first }, inner);
        Check(state.InstancePath().size() == 1 && state.InstancePath().front() == first,
              "selecting inside an instance records which copy");

        const doc::Value before = state.GetProp(inner, doc::Prop::Text);
        state.SetProp(inner, doc::Prop::Text, doc::Value{ std::string("Only this card") });
        state.EndGesture();
        driver.Frame();

        const doc::Value edited = state.GetProp(inner, doc::Prop::Text);
        Check(std::holds_alternative<std::string>(edited)
              && std::get<std::string>(edited) == "Only this card",
              "the edit reads back through the copy it was made in");

        state.SelectInside({ second }, inner);
        Check(state.GetProp(inner, doc::Prop::Text) == before,
              "the other copy of the card is untouched");

        // And what a script would call it, which is the only way to address the second of two.
        state.SelectInside({ first }, inner);
        const doc::Node* innerNode = state.Doc().Find(inner);
        Check(innerNode && state.ScriptPath(inner) == innerNode->name,
              "the inspector names the path a script addresses it by");

        state.ExitInstance();
        Check(state.InstancePath().empty() && state.Primary() == first,
              "leaving an instance selects the instance that was left");
    }


    // The dashboard's grid holds four cards, and each card holds a badge and a note. All of
    // it is the page's own writing, dropped into slots — so all of it has to behave like the
    // page's writing: pick it, edit it, undo the edit.
    void TestContainerContentsAreEditable() {
        Section("what a container holds");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample(StudioLayer::Example::Dashboard);
        driver.Frame();

        EditorState& state = layer.State();
        doc::Document& d = state.Doc();

        const auto named = [&](std::string_view name, Uuid within) {
            for (Uuid id : d.Subtree(within))
                if (const doc::Node* node = d.Find(id); node && node->name == name) return id;
            return Uuid::Invalid();
        };

        const Uuid grid = named("Figures", state.ActiveScreen());
        const Uuid card = named("New Customers", state.ActiveScreen());
        Check(grid.Valid() && card.Valid(), "the grid and its cards are nodes in the document");
        if (!grid.Valid() || !card.Valid()) return;
        Check(d.Find(card)->parent == grid, "a card in the grid is a child of the grid");

        const Uuid badge = named("Change", card);
        Check(badge.Valid(), "and the badge inside that card is a node too");
        if (!badge.Valid()) return;

        // Clicking picks the innermost thing the page wrote. Before slots there was nothing to
        // write inside a container, so a click anywhere in one gave you the container.
        const Rect badgeBox = layer.Surface().BoundsOf(state, badge);
        Check(badgeBox.size.x > 0.0f && badgeBox.size.y > 0.0f, "the badge is drawn");
        const Uuid hitBadge = layer.Surface().SelectionAt(state, badgeBox.Center());
        Check(hitBadge == badge, "clicking the badge selects the badge, not the grid");

        // A card's own title is the Card component's, and stays out of reach until you open
        // the card — the one thing that should *not* have changed.
        const Rect cardBox = layer.Surface().BoundsOf(state, card);
        const Uuid hitCard = layer.Surface().SelectionAt(
            state, { cardBox.pos.x + 3.0f, cardBox.pos.y + 3.0f });
        Check(hitCard == card, "clicking the card's own edge selects the card");

        // Editable, not merely selectable.
        state.Select(badge);
        const std::size_t children = d.Find(grid)->children.size();
        state.Select(card);
        state.DeleteSelection();
        driver.Frame();
        Check(d.Find(grid)->children.size() == children - 1,
              "deleting a card takes it out of the grid");
        state.Undo();
        driver.Frame();
        Check(d.Find(grid)->children.size() == children, "and undo puts it back");

        // And a script reaches it. The badge is on the screen the way anything else is, so the
        // screen's own script addresses it by the path the layers panel reads out loud —
        // through the card it was dropped into, not around it.
        ScriptSession& scripts = layer.Scripts();
        scripts.SetSource("vae.component(\"Dashboard\", {\n"
                           "  on_mount = function(self)\n"
                           "    self:set_text(\"Total Revenue.Change.Label\", \"text\", \"+99%\")\n"
                           "  end,\n"
                           "})\n");
        if (!Check(scripts.Build(), "a script for the dashboard builds: " + scripts.Output()))
            return;
        driver.Press(ImGuiKey_F5);
        driver.Frame();
        if (!Check(scripts.Playing(), "the dashboard runs")) return;

        // Scoped to the one card the script named: all four hold a badge called Change, and
        // a check that took any of them would pass on the wrong one.
        const ui::ViewTree& tree = layer.Surface().Host().Tree();
        const auto under = [&](u32 root, std::string_view name) {
            if (root == ui::ViewTree::kInvalid) return root;
            std::vector<u32> queue{ root };
            for (std::size_t at = 0; at < queue.size(); ++at) {
                if (queue[at] != root && tree.At(queue[at]).name == name) return queue[at];
                for (const u32 child : tree.At(queue[at]).children) queue.push_back(child);
            }
            return ui::ViewTree::kInvalid;
        };
        const u32 revenue = under(tree.Root(), "Total Revenue");
        const u32 label = revenue == ui::ViewTree::kInvalid
                        ? ui::ViewTree::kInvalid : under(under(revenue, "Change"), "Label");
        const std::string shown = label == ui::ViewTree::kInvalid
                                ? std::string{} : tree.Str(label, doc::Prop::Text);
        Check(shown == "+99%", "the script wrote through the card into the badge: " + shown);
        const u32 other = under(under(tree.Root(), "Growth Rate"), "Change");
        Check(tree.Str(under(other, "Label"), doc::Prop::Text) == "+4.5%",
              "and left the other three cards alone");

        driver.Press(ImGuiKey_F5, false, true);
        driver.Frame();

        // The block ships with no logic, and the example folder is a real one the launcher
        // reopens. Leaving this behind would mean everybody's dashboard came with a script
        // that rewrites one of its numbers.
        std::error_code ec;
        std::filesystem::remove(scripts.SourcePath(), ec);
    }


    void TestViewport() {
        Section("viewport");
        Driver driver;
        EditorState& state = driver.State();
        Canvas& canvas = driver.Surface();

        const Vec2 probe{ 321.0f, 654.0f };
        const Vec2 round = canvas.ToDocument(canvas.ToScreen(probe));
        Check(Near(round.x, probe.x, 0.01f) && Near(round.y, probe.y, 0.01f),
              "screen and document coordinates are inverses");

        canvas.FrameAll(state);
        driver.Frame();
        const Vec2 topLeft = canvas.ToScreen({ 0.0f, 0.0f });
        const Vec2 bottomRight = canvas.ToScreen({ 1280.0f, 800.0f });
        Check(topLeft.x > 0.0f && topLeft.y > 0.0f, "the framed screen's top-left is on screen");
        Check(bottomRight.x < kDisplayW && bottomRight.y < kDisplayH,
              "the framed screen's bottom-right is on screen");
        // Centred, which is what makes framing feel like framing rather than scrolling.
        Check(Near(topLeft.x, kDisplayW - bottomRight.x, 1.0f), "framing centres horizontally");

        // A screen too big for the viewport has to come back under 1:1 — the case where a
        // wrong zoom model quietly shows you a corner of the design and calls it fit.
        state.SetActiveScreen(state.AddScreen("Huge", { 4000.0f, 2500.0f }));
        driver.Frame();
        canvas.FrameAll(state);
        driver.Frame();
        Check(canvas.Zoom() < 1.0f, "framing a screen larger than the viewport zooms out");
        Check(canvas.ToScreen({ 4000.0f, 2500.0f }).x < kDisplayW, "and still fits it all in");

        // Zoom keeps the middle of the view fixed — the thing you were looking at is the thing
        // you are still looking at.
        const Vec2 centreBefore = canvas.ViewCenter();
        canvas.ZoomTo(2.0f);
        const Vec2 centreAfter = canvas.ViewCenter();
        Check(Near(centreBefore.x, centreAfter.x, 0.05f)
           && Near(centreBefore.y, centreAfter.y, 0.05f), "zooming holds the view centre");
        Check(Near(canvas.Zoom(), 2.0f, 0.001f), "the requested zoom is the zoom");
    }


    void TestShortcuts() {
        Section("shortcuts");
        Shortcuts driver;
        EditorState& state = driver.Layer_().State();

        const Uuid a = state.PlaceInstance("Button", state.ActiveScreen(), { 100.0f, 100.0f });
        Check(a.Valid(), "a widget to work on");
        const std::size_t before = state.Doc().NodeCount();

        state.Select(a);
        driver.Press(ImGuiKey_D, true);
        Check(state.Doc().NodeCount() > before, "Ctrl+D duplicates");
        const Uuid copy = state.Primary();
        Check(copy.Valid() && copy != a, "and selects the copy");

        driver.Press(ImGuiKey_Delete);
        Check(state.Doc().Find(copy) == nullptr, "Del removes the selection");

        driver.Press(ImGuiKey_Z, true);
        Check(state.Doc().Find(copy) != nullptr, "Ctrl+Z brings it back");
        driver.Press(ImGuiKey_Y, true);
        Check(state.Doc().Find(copy) == nullptr, "Ctrl+Y takes it away again");
        driver.Press(ImGuiKey_Z, true);
        driver.Press(ImGuiKey_Z, true, true);
        Check(state.Doc().Find(copy) == nullptr, "Ctrl+Shift+Z redoes, as everywhere else");

        state.Select(a);
        driver.Press(ImGuiKey_Escape);
        Check(state.Selection().empty(), "Escape drops the selection");

        const bool preview = driver.Layer_().Surface().Preview();
        driver.Press(ImGuiKey_P, true);
        Check(driver.Layer_().Surface().Preview() != preview, "Ctrl+P toggles preview");
        driver.Press(ImGuiKey_P, true);
        Check(driver.Layer_().Surface().Preview() == preview, "and toggles it back");

        driver.Press(ImGuiKey_0, true);
        Check(Near(driver.Layer_().Surface().Zoom(), 1.0f, 0.001f), "Ctrl+0 is 1:1");
    }

    // --------------------------------------------------------------------------- play mode


    // Editing a label on the canvas instead of in the Inspector.
    void TestInlineTextEdit() {
        Section("edit text on the canvas");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        doc::Document& d = state.Doc();
        Canvas& canvas = layer.Surface();

        const Uuid label = d.CreateNode(doc::NodeKind::Text, state.ActiveScreen(), "Label");
        d.SetProp(label, doc::Prop::Text, std::string("Before"));
        driver.Frame();

        canvas.BeginTextEdit(state, label);
        driver.Frame();
        Check(canvas.EditingText() == label, "double-clicking a label starts editing it");
        Check(state.Selection().size() == 1 && state.Primary() == label,
              "and selects what is being edited");

        // Typing goes through the same SetProp the Inspector uses, so it is undoable.
        state.SetProp(label, doc::Prop::Text, std::string("After"));
        state.EndGesture();
        canvas.EndTextEdit();
        driver.Frame();
        Check(canvas.EditingText() == Uuid::Invalid(), "and it ends when the field goes away");
        Check(d.GetProp(label, doc::Prop::Text) == doc::Value{ std::string("After") },
              "the label kept what was typed");
        state.Undo();
        Check(d.GetProp(label, doc::Prop::Text) == doc::Value{ std::string("Before") },
              "and undo puts back what it said");

        // A frame is not a label: double-clicking one must not open a text field over it.
        const Uuid frame = d.CreateNode(doc::NodeKind::Frame, state.ActiveScreen(), "Box");
        driver.Frame();
        canvas.BeginTextEdit(state, frame);
        Check(canvas.EditingText() == Uuid::Invalid(), "a frame has no text to edit");
    }


    // Copy, paste and a deep duplicate. The clipboard is markup, so this also checks that a
    // subtree survives being written out and read back — the same trip a saved file makes.
    void TestClipboard() {
        Section("clipboard");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        doc::Document& d = state.Doc();

        // A card with two labels in it: the case a shallow copy gets wrong.
        const Uuid card = d.CreateNode(doc::NodeKind::Frame, state.ActiveScreen(), "Card");
        {
            doc::Node* node = d.Find(card);
            node->layout.mode = layout::LayoutMode::Stack;
            node->layout.axis = layout::Axis::Column;
            node->layout.width = layout::Size::Px(200.0f);
            node->layout.height = layout::Size::Hug();
            d.Touch(card);
        }
        const Uuid title = d.CreateNode(doc::NodeKind::Text, card, "Title");
        d.SetProp(title, doc::Prop::Text, std::string("Hello"));
        d.CreateNode(doc::NodeKind::Text, card, "Body");
        driver.Frame();

        const std::size_t before = d.NodeCount();

        // Duplicate is deep: a card duplicated without its labels is an empty card, which is
        // exactly what it used to produce.
        state.Select(card);
        state.DuplicateSelection();
        driver.Frame();
        const Uuid copy = state.Primary();
        Check(copy.Valid() && copy != card, "duplicate made a copy");
        Check(d.Find(copy) && d.Find(copy)->children.size() == 2,
              "and it brought both children with it");
        Check(d.NodeCount() == before + 3, "three nodes, not one");
        state.Undo();
        driver.Frame();
        Check(d.NodeCount() == before, "undo takes the whole subtree back out");

        // Copy to the clipboard and paste it back.
        state.Select(card);
        state.CopySelection();
        const std::string clipboard = Input::ClipboardText();
        Check(clipboard.find("<vae") != std::string::npos, "the clipboard holds markup");
        Check(clipboard.find("Title") != std::string::npos, "with the children in it");
        Check(state.CanPaste(), "and Studio can tell it is pasteable");

        state.Select(state.ActiveScreen());
        const u32 pasted = state.Paste();
        driver.Frame();
        Check(pasted == 1, "pasting brought one root back");
        const Uuid landed = state.Primary();
        Check(landed.Valid() && landed != card, "under an id of its own");
        Check(d.Find(landed) && d.Find(landed)->children.size() == 2, "with its children");
        const Uuid pastedTitle = d.Find(landed)->children.front();
        Check(d.GetProp(pastedTitle, doc::Prop::Text) == doc::Value{ std::string("Hello") },
              "and the text it had");

        // Pasting twice must not produce two nodes claiming one id.
        const u32 again = state.Paste();
        driver.Frame();
        Check(again == 1 && state.Primary() != landed, "a second paste is a second copy");

        state.Undo();
        driver.Frame();
        Check(d.Find(landed) != nullptr, "and undo removes only the last one");

        // Cut takes it away and leaves it on the clipboard.
        state.Select(landed);
        state.CutSelection();
        driver.Frame();
        Check(d.Find(landed) == nullptr, "cut removed it");
        Check(state.CanPaste(), "and it is still on the clipboard");
    }


    // Files dragged in from the desktop. Driven through the same handler the window callback
    // calls, because nothing else can simulate a drag from a file manager.
    void TestFileDrop() {
        Section("dropped files");
        Shortcuts driver;
        StudioLayer& layer = driver.Layer_();
        layer.OpenExample();
        driver.Frame();

        EditorState& state = layer.State();
        const std::size_t before = state.Doc().Assets().size();

        // The engine's own icon: a real file, of a kind the asset store handles.
        const std::filesystem::path icon = FileSystem::Asset("VAE/assets/icon.svg");
        layer.OnFilesDropped({ icon });
        driver.Frame();
        Check(state.Doc().Assets().size() == before + 1, "a dropped picture becomes an asset");
        // "icon", or "icon-2" when the project already has one — importing the same file twice
        // must not overwrite the first copy, so the name is uniquified.
        const bool named = std::any_of(state.Doc().Assets().begin(), state.Doc().Assets().end(),
                                       [](const doc::Document::Asset& a) {
                                           return a.name.rfind("icon", 0) == 0;
                                       });
        Check(named, "under the name of the file it came from");

        // Undoable like any other edit, because that is what importing the wrong file needs.
        state.Undo();
        driver.Frame();
        Check(state.Doc().Assets().size() == before, "and dropping it is undoable");

        // A document in the drop opens instead of being imported as a picture.
        const std::filesystem::path project = FileSystem::ProjectsRoot() / "Counter example"
                                            / "Counter example.vae";
        if (std::filesystem::exists(project)) {
            layer.OnFilesDropped({ project });
            driver.Frame();
            Check(layer.State().Path() == project, "a dropped .vae file opens the project");
            Check(state.Doc().Assets().size() != before + 2,
                  "rather than being imported as an asset");
        }
    }

    void RunDirect() {
        TestHitTest();
        TestDrag();
        TestSnap();
        TestResize();
        TestMarquee();
        TestViewport();
        TestDeleteAndDuplicate();
        TestAlignAndDistribute();
        TestShortcuts();
        TestGrouping();
        TestInlineTextEdit();
        TestClipboard();
        TestFileDrop();
        TestContainerContentsAreEditable();
        TestNesting();
        TestFillingWidgetMoves();
        TestDroppingIntoAContainer();
        TestPlacementUndoes();
    }

}
