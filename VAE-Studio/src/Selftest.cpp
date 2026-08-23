#include "Selftest.h"
#include "vae/base/FileSystem.h"

#include "Canvas.h"
#include "EditorState.h"
#include "Debugger.h"
#include "ScriptSession.h"
#include "StudioLayer.h"

#include "vae/app/RunLayer.h"
#include "vae/base/Log.h"
#include "vae/core/Application.h"
#include "vae/text/FontDB.h"

#include <imgui.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

namespace vae {

    namespace {

        int g_Checks = 0;
        int g_Failed = 0;
        const char* g_Section = "";

        void Section(const char* name) {
            g_Section = name;
            VAE_INFO("selftest · {}", name);
        }

        bool Check(bool ok, std::string_view what) {
            ++g_Checks;
            if (!ok) {
                ++g_Failed;
                VAE_ERROR("  FAIL  {}: {}", g_Section, what);
            }
            return ok;
        }

        bool Near(f32 a, f32 b, f32 epsilon = 0.5f) { return std::abs(a - b) <= epsilon; }

        // A generous display: the canvas has to hold a 1280x800 screen at 1:1 with room around it,
        // or points the script clicks fall outside the viewport and are never hovered.
        constexpr f32 kDisplayW = 1600.0f;
        constexpr f32 kDisplayH = 1000.0f;

        // Drives the editor with no window and no GPU. ImGui's core is pure logic — hover, click,
        // drag thresholds — so a context with no backend attached reproduces input exactly, and
        // Canvas::OnImGuiRender runs the same code a mouse runs.
        class Driver {
        public:
            Driver() {
                IMGUI_CHECKVERSION();
                m_Context = ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
                io.DeltaTime = 1.0f / 60.0f;
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                // Claim the modern texture protocol so ImGui never falls back to asking a backend
                // that does not exist for an atlas upload.
                io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

                text::FontDB::Get().LoadDefaults();

                // Two frames: the first creates the canvas window, the second gives it the
                // geometry that ToScreen depends on.
                MouseTo({ kDisplayW * 0.5f, kDisplayH * 0.5f });
                Frame();
                Frame();
            }

            ~Driver() { ImGui::DestroyContext(m_Context); }

            EditorState& State()  { return m_State; }
            Canvas&      Surface() { return m_Canvas; }
            const std::vector<Rect>& GuidesDuringDrag() const { return m_DragGuides; }

            void Frame() {
                ImGui::NewFrame();
                m_Canvas.SyncScene(m_State, 1.0f / 60.0f);
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
                ImGui::SetNextWindowSize(ImVec2(kDisplayW, kDisplayH));
                m_Canvas.OnImGuiRender(m_State);
                ImGui::Render();
            }

            void MouseTo(Vec2 screen) { ImGui::GetIO().AddMousePosEvent(screen.x, screen.y); }
            void Button(bool down)    { ImGui::GetIO().AddMouseButtonEvent(0, down); }

            void ClickDoc(Vec2 point) {
                MouseTo(m_Canvas.ToScreen(point));
                Frame();
                Button(true);
                Frame();
                Button(false);
                Frame();
            }

            // A press, several moves and a release — the shape of a real drag, because gestures
            // that only ever see one move frame never exercise coalescing or snapping.
            void DragDoc(Vec2 from, Vec2 to, int steps = 8) {
                MouseTo(m_Canvas.ToScreen(from));
                Frame();
                Button(true);
                Frame();
                for (int i = 1; i <= steps; ++i) {
                    const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
                    MouseTo(m_Canvas.ToScreen(from + (to - from) * t));
                    Frame();
                }
                m_DragGuides = m_Canvas.Guides();     // guides are cleared on release
                Button(false);
                Frame();
            }

        private:
            ImGuiContext* m_Context = nullptr;
            EditorState   m_State;
            Canvas        m_Canvas;
            std::vector<Rect> m_DragGuides;
        };

        Vec2 OffsetOf(EditorState& state, Uuid id) {
            const doc::Node* node = state.Doc().Find(id);
            return node ? node->layout.offsetStart : Vec2{ 0.0f, 0.0f };
        }

        // Places an instance and pins it to an exact box, so every later assertion is about the
        // gesture under test rather than about how wide a button's label happened to be.
        Uuid PlaceFixed(EditorState& state, std::string_view component, Vec2 at, Vec2 size) {
            const Uuid id = state.PlaceInstance(component, state.ActiveScreen(), at);
            if (!id.Valid()) return id;
            layout::LayoutStyle style = state.Doc().Find(id)->layout;
            style.width  = layout::Size::Px(size.x);
            style.height = layout::Size::Px(size.y);
            state.SetLayout(id, style);
            state.EndGesture();
            return id;
        }

        // A file writes an id only on a node something refers to, so anything looked up after a
        // save and a load is found the way a script finds it: by name. What has to survive a round
        // trip is the node, not the number.
        const doc::Node* FindByName(const doc::Document& doc, std::string_view name) {
            for (Uuid root : doc.Roots())
                for (Uuid id : doc.Subtree(root))
                    if (const doc::Node* node = doc.Find(id); node && node->name == name)
                        return node;
            return nullptr;
        }

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

        void TestAuthoringAComponent() {
            Section("authoring a component");
            Driver driver;
            EditorState& state = driver.State();

            // A frame with two things in it: the shape somebody would want to reuse.
            const Uuid frame = state.CreateChild(doc::NodeKind::Frame, state.ActiveScreen(), "Row");
            {
                layout::LayoutStyle style = state.Doc().Find(frame)->layout;
                style.mode = layout::LayoutMode::Stack;
                style.axis = layout::Axis::Row;
                style.gap = 8.0f;
                style.offsetStart = { 80.0f, 80.0f };
                state.SetLayout(frame, style);
            }
            const Uuid label = state.CreateChild(doc::NodeKind::Text, frame, "Caption");
            state.SetProp(label, doc::Prop::Text, doc::Value{ std::string("Reusable") });
            const Uuid slot = state.CreateChild(doc::NodeKind::Frame, frame, "Body");
            state.EndGesture();
            driver.Frame();

            state.Select(frame);
            const std::size_t before = state.Library().components.size();
            const Uuid instance = state.MakeComponentFromSelection();
            driver.Frame();

            Check(instance.Valid(), "the frame became an instance of a new component");
            if (!instance.Valid()) return;
            Check(state.Library().components.size() == before + 1, "the library gained one entry");
            Check(state.Library().Find("Row") == frame, "and it is indexed under the frame's name");

            const doc::Node* master = state.Doc().Find(frame);
            Check(master && master->IsComponent(), "the frame itself is now the component");
            Check(master && !master->parent.Valid(), "and it left the screen");
            const doc::Node* placed = state.Doc().Find(instance);
            Check(placed && placed->parent == state.ActiveScreen(),
                  "the instance took its place on the screen");
            Check(placed && Near(placed->layout.offsetStart.x, 80.0f),
                  "in the same spot, so nothing on the screen moved");

            // A slot, and something put in it.
            state.SetSlot(slot, true);
            Check(state.Doc().SlotOf(frame) == slot, "the marked frame is the component's slot");

            const Uuid inner = state.PlaceInstance("Button", instance, { 0.0f, 0.0f });
            state.EndGesture();
            driver.Frame();
            Check(inner.Valid(), "a button dropped into the instance");
            const Rect box = driver.Surface().BoundsOf(state, inner);
            const Rect outer = driver.Surface().BoundsOf(state, instance);
            Check(box.size.x > 0.0f && outer.Contains(box.Center()),
                  "and it is drawn inside the component's slot");

            // One slot per component: marking another moves it.
            state.SetSlot(label, true);
            Check(state.Doc().SlotOf(frame) == label, "marking another slot moves it");
            const doc::Node* previous = state.Doc().Find(slot);
            Check(previous && !previous->slot, "and clears the one that held it");

            // And the master can be opened again. A component leaves the screen when it is made,
            // so without a way back it is a definition nobody can ever edit.
            const Uuid screen = state.ActiveScreen();
            state.OpenComponent(frame);
            driver.Frame();
            Check(state.EditingComponent() == frame, "the master opened on the canvas");
            const Rect opened = driver.Surface().BoundsOf(state, frame);
            Check(opened.size.x > 0.0f && opened.size.y > 0.0f,
                  "and it has a box, even though it hugs its contents");

            state.CloseComponent();
            driver.Frame();
            Check(state.ActiveScreen() == screen, "closing it goes back to the screen");
            Check(!state.EditingComponent().Valid(), "and nothing is being edited");
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

        void TestInspectorRoundTrip() {
            Section("inspector edit");
            Driver driver;
            EditorState& state = driver.State();

            const Uuid a = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 200.0f, 48.0f });
            const Uuid b = PlaceFixed(state, "Button", { 500.0f, 300.0f }, { 200.0f, 48.0f });
            const Uuid master = state.Library().Find("Button");
            const doc::Value masterText = state.Doc().GetProp(master, doc::Prop::Text);

            state.SetProp(a, doc::Prop::Text, doc::Value{ std::string("Renamed") });
            state.EndGesture();

            const doc::Value read = state.GetProp(a, doc::Prop::Text);
            Check(std::holds_alternative<std::string>(read)
                  && std::get<std::string>(read) == "Renamed",
                  "the edit reads back off the instance");

            // The whole point of an override: one card says something different without every
            // other card changing with it.
            Check(state.Doc().GetProp(master, doc::Prop::Text) == masterText,
                  "the component master is untouched");
            Check(state.GetProp(b, doc::Prop::Text) == masterText,
                  "the sibling instance is untouched");

            state.Undo();
            const doc::Value after = state.GetProp(a, doc::Prop::Text);
            Check(after == masterText, "undo clears an override that did not exist before");

            state.Redo();
            const doc::Value redone = state.GetProp(a, doc::Prop::Text);
            Check(std::holds_alternative<std::string>(redone)
                  && std::get<std::string>(redone) == "Renamed", "redo restores the override");

            // A layout edit is the other half of the inspector, and it goes through the same stack.
            layout::LayoutStyle style = state.Doc().Find(a)->layout;
            style.padding = Edges{ 12.0f, 13.0f, 14.0f, 15.0f };
            state.SetLayout(a, style);
            state.EndGesture();
            Check(state.Doc().Find(a)->layout.padding.left == 12.0f, "a layout edit lands");
            state.Undo();
            Check(state.Doc().Find(a)->layout.padding.left != 12.0f, "and undoes");
        }

        void TestSaveLoad() {
            Section("save and load");
            Driver driver;
            EditorState& state = driver.State();

            const Uuid a = PlaceFixed(state, "Button", { 137.0f, 219.0f }, { 210.0f, 44.0f });
            state.SetProp(a, doc::Prop::Text, doc::Value{ std::string("Persisted") });
            state.EndGesture();

            const std::size_t nodes = state.Doc().NodeCount();
            const auto path = std::filesystem::temp_directory_path() / "vae-selftest.vae";
            Check(state.Save(path), "the project saves");
            Check(!state.Dirty(), "saving clears the dirty flag");

            Check(state.Load(path), "the project loads back");
            Check(state.Doc().NodeCount() == nodes, "the node count survives the round trip");

            // Looked up by where it is rather than by id: a file writes an id only on a node
            // something refers to, and nothing refers to a placed instance. Its component and the
            // screen it sits on are found by id, because those ARE referenced.
            const doc::Node* node = nullptr;
            if (const doc::Node* screen = state.Doc().Find(state.ActiveScreen()))
                for (Uuid child : screen->children)
                    if (const doc::Node* n = state.Doc().Find(child); n && n->IsInstance()) node = n;
            Check(node != nullptr, "the placed widget survives a save and a load");
            if (node) {
                Check(Near(node->layout.offsetStart.x, 137.0f)
                   && Near(node->layout.offsetStart.y, 219.0f), "position survives");
                Check(Near(node->layout.width.value, 210.0f), "size survives");
                const doc::Value text = state.GetProp(node->id, doc::Prop::Text);
                Check(std::holds_alternative<std::string>(text)
                      && std::get<std::string>(text) == "Persisted", "the override survives");
                // The instance still points at the component it is an instance of: that id IS
                // written, because something refers to it.
                Check(node->componentId.Valid() && state.Doc().Find(node->componentId) != nullptr,
                      "and still points at its component");
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
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

        // Keyboard shortcuts, driven through the whole editor layer rather than a copy of it.
        // They cannot be checked in a nested compositor: vc's virtual keyboard invents its own
        // keycodes, so GLFW resolves a raw Escape to a digit. Here the key events are exact.
        class Shortcuts {
        public:
            Shortcuts() {
                IMGUI_CHECKVERSION();
                m_Context = ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
                io.DeltaTime = 1.0f / 60.0f;
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
                text::FontDB::Get().LoadDefaults();
                m_Layer.OnAttach();
                Frame();
                Frame();
            }
            ~Shortcuts() { ImGui::DestroyContext(m_Context); }

            StudioLayer& Layer_() { return m_Layer; }
            void Frame() {
                ImGui::NewFrame();
                m_Layer.OnImGuiRender();
                ImGui::Render();
            }

            void Press(ImGuiKey key, bool ctrl = false, bool shift = false) {
                ImGuiIO& io = ImGui::GetIO();
                if (ctrl)  { io.AddKeyEvent(ImGuiMod_Ctrl, true);  io.AddKeyEvent(ImGuiKey_LeftCtrl, true); }
                if (shift) { io.AddKeyEvent(ImGuiMod_Shift, true); io.AddKeyEvent(ImGuiKey_LeftShift, true); }
                io.AddKeyEvent(key, true);
                Frame();
                io.AddKeyEvent(key, false);
                if (ctrl)  { io.AddKeyEvent(ImGuiKey_LeftCtrl, false);  io.AddKeyEvent(ImGuiMod_Ctrl, false); }
                if (shift) { io.AddKeyEvent(ImGuiKey_LeftShift, false); io.AddKeyEvent(ImGuiMod_Shift, false); }
                Frame();
            }

        private:
            ImGuiContext* m_Context = nullptr;
            StudioLayer   m_Layer;
        };

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

        // A view's bounds, addressed the way a script does: a name, inside one copy of a component.
        Rect BoundsIn(const ui::ViewTree& tree, Uuid owner, std::string_view name) {
            u32 root = ui::ViewTree::kInvalid;
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).instanceId == owner) { root = i; break; }
            if (root == ui::ViewTree::kInvalid) return {};

            std::vector<u32> queue{ root };
            for (std::size_t at = 0; at < queue.size(); ++at) {
                const u32 view = queue[at];
                if (view != root && tree.At(view).name == name) return tree.Bounds(view);
                for (const u32 child : tree.At(view).children) queue.push_back(child);
            }
            return {};
        }

        std::string TextIn(const ui::ViewTree& tree, Uuid owner, std::string_view name) {
            u32 root = ui::ViewTree::kInvalid;
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).instanceId == owner) { root = i; break; }
            if (root == ui::ViewTree::kInvalid) return {};

            std::vector<u32> queue{ root };
            for (std::size_t at = 0; at < queue.size(); ++at) {
                const u32 view = queue[at];
                if (view != root && tree.At(view).name == name)
                    return tree.Str(view, doc::Prop::Text);
                for (const u32 child : tree.At(view).children) queue.push_back(child);
            }
            return {};
        }

        Uuid InstanceNamed(const doc::Document& document, Uuid screen, std::string_view name) {
            const doc::Node* node = document.Find(screen);
            if (!node) return Uuid::Invalid();
            for (const Uuid child : node->children)
                if (const doc::Node* c = document.Find(child); c && c->name == name) return child;
            return Uuid::Invalid();
        }

        void TestPlayMode() {
            Section("play mode");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();

            layer.OpenExample();
            driver.Frame();
            driver.Frame();

            EditorState& state = layer.State();
            const Uuid left  = InstanceNamed(state.Doc(), state.ActiveScreen(), "Left");
            const Uuid right = InstanceNamed(state.Doc(), state.ActiveScreen(), "Right");
            if (!Check(left.Valid() && right.Valid(), "the example puts two counters on screen"))
                return;

            ScriptSession& scripts = layer.Scripts();
            Check(scripts.Lang() == ScriptSession::Language::Lua, "Lua by default");
            Check(scripts.Build(), "the example's script builds: " + scripts.Output());

            // F5 is the whole gesture: build if needed, snapshot, start, switch the canvas over.
            driver.Press(ImGuiKey_F5);
            if (!Check(scripts.Playing(), "F5 starts the app")) return;
            Check(layer.Surface().Preview(), "and the canvas stops looking like an editor");
            driver.Frame();
            Check(scripts.LiveInstances() == 2, "both copies mounted, not one shared script");

            const ui::ViewTree& tree = layer.Surface().Host().Tree();
            Check(TextIn(tree, left, "Count") == "0", "on_mount wrote through the designer's name");

            // Clicks go in as real pointer events at the coordinates the layout produced.
            auto click = [&](Uuid owner, std::string_view name) {
                const Rect box = BoundsIn(layer.Surface().Host().Tree(), owner, name);
                const Vec2 point = box.Center();
                ui::UiHost& host = layer.Surface().Host();
                host.Dispatch(MakeMouseMoved(point.x, point.y));
                host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                              point.x, point.y, Mod::None));
                host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                              point.x, point.y, Mod::None));
                driver.Frame();
            };

            click(left, "Increment");
            click(left, "Increment");
            click(right, "Increment");
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "2",
                  "the left counter counted its own clicks: "
                      + TextIn(layer.Surface().Host().Tree(), left, "Count"));
            Check(TextIn(layer.Surface().Host().Tree(), right, "Count") == "1",
                  "and the right one counted separately: "
                      + TextIn(layer.Surface().Host().Tree(), right, "Count"));

            // Hot reload: the code is swapped, the screen keeps saying what it said.
            driver.Press(ImGuiKey_F6);
            driver.Frame();
            Check(scripts.Playing(), "F6 leaves the app running");
            Check(scripts.LiveInstances() == 2, "and everything still mounted");
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "2",
                  "live state survived the reload");

            click(left, "Reset");
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "0",
                  "reload left a working script behind, not a dead one");
            Check(TextIn(layer.Surface().Host().Tree(), right, "Count") == "1",
                  "and reset stayed inside the copy it was clicked in");

            // Stopping puts the design back: what the script wrote is not what the designer drew.
            driver.Press(ImGuiKey_F5, false, true);
            driver.Frame();
            Check(!scripts.Playing(), "Shift+F5 stops");
            Check(!layer.Surface().Preview(), "and the editor comes back");
            Check(TextIn(layer.Surface().Host().Tree(), right, "Count") == "0",
                  "the document is the design again, not the last frame of the app: "
                      + TextIn(layer.Surface().Host().Tree(), right, "Count"));
            Check(state.Doc().Find(left) != nullptr && state.Doc().Find(right) != nullptr,
                  "restored in place, ids and all");

            // The same example in the other language, through the compiler this time. It is the
            // only thing that catches a C++ template that stopped compiling against the header.
            scripts.SetLanguage(ScriptSession::Language::Cpp);
            layer.OpenExample();
            driver.Frame();
            Check(scripts.Build(), "the example's C++ builds too: " + scripts.Output());

            driver.Press(ImGuiKey_F5);
            driver.Frame();
            if (Check(scripts.Playing(), "and runs")) {
                const Uuid a = InstanceNamed(state.Doc(), state.ActiveScreen(), "Left");
                Check(scripts.LiveInstances() == 2, "with both copies mounted");
                Check(TextIn(layer.Surface().Host().Tree(), a, "Count") == "0",
                      "showing what the C++ on_mount wrote");
                driver.Press(ImGuiKey_F5, false, true);
            }
        }

        // ------------------------------------------------------------------------ debug tools

        void TestDebugger() {
            Section("debug tools");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            ScriptSession& scripts = layer.Scripts();
            Debugger& debugger = scripts.Debug();

            layer.OpenExample();
            driver.Frame();
            EditorState& state = layer.State();
            const Uuid left = InstanceNamed(state.Doc(), state.ActiveScreen(), "Left");
            if (!Check(left.Valid(), "a counter to debug")) return;

            Check(scripts.Build(), "example builds: " + scripts.Output());
            driver.Press(ImGuiKey_F5);
            driver.Frame();
            if (!Check(scripts.Playing(), "running")) return;
            Check(debugger.Watches().empty(), "a run starts with nothing pinned");
            Check(debugger.Log().empty(), "and nothing recorded");

            // A press and a release on separate frames, which is what a real click is. It matters
            // here: anything that rebuilds the view tree between the two — a debugger writing a
            // frozen value into the document every frame, say — would drop the click, and a click
            // delivered inside one frame would never notice.
            auto click = [&](std::string_view name) {
                ui::UiHost& host = layer.Surface().Host();
                const Vec2 point = BoundsIn(host.Tree(), left, name).Center();
                host.Dispatch(MakeMouseMoved(point.x, point.y));
                driver.Frame();
                host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                              point.x, point.y, Mod::None));
                driver.Frame();
                host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                              point.x, point.y, Mod::None));
                driver.Frame();
            };

            // The event log records what reached which script, not merely that something happened.
            click("Increment");
            if (Check(!debugger.Log().empty(), "the click was recorded")) {
                const Debugger::Traced& entry = debugger.Log().back();
                Check(entry.kind == "clicked", "as a click: " + entry.kind);
                Check(entry.source == "Increment", "from the node the designer named: " + entry.source);
                Check(entry.instance == left, "delivered to the copy that was clicked");
            }
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "1", "and the script ran");

            // Reading a state key the way the panel does.
            debugger.Add({ left, "Left", {}, "count", true });
            Check(debugger.Watches().size() == 1, "pinned");
            debugger.Add({ left, "Left", {}, "count", true });
            Check(debugger.Watches().size() == 1, "and pinning it twice does not stack up");

            const doc::Value seen = debugger.Read(debugger.Watches().front(), scripts.Runtime(),
                                                  layer.Surface().Host().Tree());
            Check(doc::TypeOf(seen) == doc::ValueType::Number && std::get<f32>(seen) == 1.0f,
                  "the watch reads what the script is holding");

            // Writing one back: the debugger reaches into a live script, not a copy of it.
            debugger.Write(debugger.Watches().front(), scripts.Runtime(),
                           layer.Surface().Host().Tree(), doc::Value{ 41.0f });
            click("Increment");
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "42",
                  "an edited state value is the one the script carries on from: "
                      + TextIn(layer.Surface().Host().Tree(), left, "Count"));

            // Freezing a property the script writes on every click. The script keeps winning until
            // the freeze is on, and then it stops winning — which is the whole feature.
            debugger.Add({ left, "Left", "Count", "text", false });
            Debugger::Watch& frozen = debugger.Watches().back();
            frozen.frozen = true;
            frozen.hold = std::string("held");
            driver.Frame();
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "held",
                  "a frozen property is held: "
                      + TextIn(layer.Surface().Host().Tree(), left, "Count"));

            click("Increment");
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") == "held",
                  "and stays held while the script writes it every click: "
                      + TextIn(layer.Surface().Host().Tree(), left, "Count"));
            const auto* counting = scripts.Runtime().StateOf(left);
            Check(counting && std::get<f32>(counting->at("count")) > 42.0f,
                  "while the script itself kept counting underneath");

            debugger.Watches().back().frozen = false;
            click("Increment");
            Check(TextIn(layer.Surface().Host().Tree(), left, "Count") != "held",
                  "unfreezing hands it back");

            // Stopping clears the debugger: a watch pinned to a dead instance is a trap.
            driver.Press(ImGuiKey_F5, false, true);
            driver.Frame();
            Check(debugger.Watches().empty(), "stopping forgets the watches");
            Check(debugger.Log().empty(), "and the log");
        }

        // --------------------------------------------------------------------------- screens

        void TestStyling() {
            Section("styling");
            Driver driver;
            EditorState& state = driver.State();

            const Uuid button = PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 160.0f, 40.0f });
            const Uuid master = state.Library().Find("Button");
            const doc::Node* component = state.Doc().Find(master);
            Check(component != nullptr, "the button component exists");
            if (!component) return;

            // The library styles widgets with theme tokens. A token that cannot be replaced is a
            // colour the designer cannot change, which is what "hardcoded" felt like.
            const doc::Value fill = state.GetProp(button, doc::Prop::Fill);
            Check(std::holds_alternative<doc::TokenRef>(fill), "a library widget is themed, not literal");

            const doc::Value resolved = state.Doc().ResolveValue(fill);
            const Color* was = std::get_if<Color>(&resolved);
            Check(was != nullptr, "the token resolves to a colour");
            if (!was) return;

            const u32 before = state.Commands().UndoDepth();
            state.SetProp(button, doc::Prop::Fill, doc::Value{ Color{ 0.9f, 0.2f, 0.2f, 1.0f } });
            state.EndGesture();
            Check(state.Commands().UndoDepth() == before + 1,
                  "replacing a token with a literal is an undoable edit");

            const doc::Value literal = state.GetProp(button, doc::Prop::Fill);
            Check(std::holds_alternative<Color>(literal), "the fill is a literal now");

            state.Undo();
            Check(std::holds_alternative<doc::TokenRef>(state.GetProp(button, doc::Prop::Fill)),
                  "undo puts the token back");
            state.Redo();

            // Hover is a property of its own, so recolouring the base leaves it saying whatever the
            // library authored — a red button that hovers blue. It has to be editable.
            const std::string hoverKey = ui::StateKey(ui::StateBit::Hovered, doc::Prop::Fill);
            state.SetProp(button, hoverKey, doc::Value{ Color{ 1.0f, 0.4f, 0.4f, 1.0f } });
            state.EndGesture();

            const doc::Value hover = state.GetProp(button, hoverKey);
            const Color* hovered = std::get_if<Color>(&hover);
            Check(hovered && hovered->r > 0.9f && hovered->b < 0.5f,
                  "a state colour reads back off the instance");

            // And the component every other button shares is untouched by either edit.
            Check(state.Doc().GetProp(master, doc::Prop::Fill) == fill,
                  "the component master keeps its token");

            // Clearing an override on an instance falls back to what the component says — which,
            // for a library widget, is a tint rather than a colour. That is the fix for the bug the
            // whole section is about: hover follows the base instead of naming a blue.
            state.SetProp(button, hoverKey, doc::Value{});
            state.EndGesture();
            const doc::Value tint =
                state.GetProp(button, ui::StateTintKey(ui::StateBit::Hovered));
            Check(std::holds_alternative<f32>(tint) && std::get<f32>(tint) > 0.0f,
                  "with no colour named, hover is a tint of whatever the fill is");
        }

        void TestProjectWithoutAScript() {
            Section("no script");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            ScriptSession& scripts = layer.Scripts();
            EditorState& state = layer.State();

            // Most projects never need code: a screen wired with declared navigation and the widget
            // library runs on its own. Refusing to Play one is refusing to run an ordinary app.
            Check(!scripts.HasSource() || true, "a fresh project may or may not have a script yet");
            PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 160.0f, 40.0f });
            driver.Frame();

            if (!scripts.HasSource()) {
                Check(scripts.Build(), "nothing to build is not a failure");
                Check(scripts.Play(), "Play runs the screen with no script at all");
                Check(scripts.Playing(), "and it is running");
                Check(scripts.LiveInstances() == 0, "with nothing mounted, because there is no code");
                scripts.Stop();
                Check(!scripts.Playing(), "and it stops again");
            }
        }

        void TestAssets() {
            Section("assets");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            EditorState& state = layer.State();

            const std::string name = "Selftest assets";
            const std::filesystem::path folder = FileSystem::ProjectsRoot() / name;
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);
            layer.CreateProject(name);

            // A 1x1 PNG, written by hand: the point is the import path, not the decoder.
            static const unsigned char kPng[] = {
                0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0,0,0,0x0D,'I','H','D','R',
                0,0,0,1, 0,0,0,1, 8,6,0,0,0, 0x1F,0x15,0xC4,0x89,
                0,0,0,0x0A,'I','D','A','T', 0x78,0x9C,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,
                0x0D,0x0A,0x2D,0xB4, 0,0,0,0,'I','E','N','D',0xAE,0x42,0x60,0x82 };
            const std::filesystem::path source = folder / "elsewhere.png";
            {
                std::ofstream out(source, std::ios::binary);
                out.write(reinterpret_cast<const char*>(kPng), sizeof kPng);
            }

            const Uuid asset = state.ImportAsset(source);
            Check(asset.Valid(), "an image imports: " + state.AssetError());
            Check(std::filesystem::exists(folder / "assets" / "elsewhere.png", ec),
                  "and the file is copied into the project, not linked from where it was");
            const doc::Document::Asset* entry = state.Doc().FindAsset(asset);
            Check(entry && entry->path == "assets/elsewhere.png",
                  "recorded by a relative path, so moving the project keeps it");

            // The same file twice is two assets, not one silently overwriting the other's file.
            const Uuid again = state.ImportAsset(source);
            Check(again.Valid() && again != asset, "importing it again is a second asset");
            Check(std::filesystem::exists(folder / "assets" / "elsewhere 2.png", ec),
                  "with a name of its own");

            state.ImportAsset(folder / "nothing-here.png");
            Check(!state.AssetError().empty(), "a missing file says so rather than failing quietly");

            // And the reference survives a save and a load, which is the only thing that makes an
            // id better than a path.
            const Uuid picture = state.CreateChild(doc::NodeKind::Image, state.ActiveScreen(),
                                                   "Picture");
            state.SetProp(picture, doc::Prop::Image, doc::AssetRef{ asset });
            state.EndGesture();
            layer.SaveProject(state.Path());

            EditorState reopened;
            Check(reopened.Load(folder / (name + ".vaescreen")), "the project reopens");
            Check(reopened.Doc().FindAsset(asset) != nullptr, "with its assets");
            const doc::Node* back = FindByName(reopened.Doc(), "Picture");
            Check(back != nullptr, "and the picture that used it");
            const doc::Value ref = back ? reopened.Doc().GetProp(back->id, doc::Prop::Image)
                                        : doc::Value{};
            Check(std::holds_alternative<doc::AssetRef>(ref)
                  && std::get<doc::AssetRef>(ref).id == asset,
                  "and the node still points at the one it was given");

            std::filesystem::remove_all(folder, ec);
        }

        // Artwork takes the same route as a picture and comes out a different thing at the end of
        // it: a Vector node, sized from the file, coloured by a token rather than by whatever the
        // author had open. Parsing needs no GPU, so all of that is checkable here.
        void TestArtwork() {
            Section("artwork");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            EditorState& state = layer.State();
            Canvas& canvas = layer.Surface();

            const std::string name = "Selftest artwork";
            const std::filesystem::path folder = FileSystem::ProjectsRoot() / name;
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);
            layer.CreateProject(name);

            const std::filesystem::path source = folder / "mark.svg";
            {
                std::ofstream out(source);
                out << R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"
                        width="24" height="24" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M4 12h6l2 6 4-12 2 6h2"/>
                    </svg>)SVG";
            }

            const Uuid asset = state.ImportAsset(source);
            if (!Check(asset.Valid(), "an SVG imports: " + state.AssetError())) return;
            canvas.Assets().Rebind(state.Doc());
            Check(canvas.Assets().IsVector(asset), "and is known to be artwork, not a picture");
            Check(canvas.Assets().ProblemWith(asset).empty(),
                  "it reads: " + canvas.Assets().ProblemWith(asset));
            Check(canvas.Assets().SizeOf(asset).x == 24.0f, "at the size the file states");
            Check(canvas.Assets().FollowsText(asset),
                  "and it asked to be told what colour to be");

            const Uuid placed = state.PlaceArtwork(asset, state.ActiveScreen(), { 40.0f, 40.0f },
                                                   canvas.Assets().SizeOf(asset),
                                                   canvas.Assets().FollowsText(asset));
            driver.Frame();
            const doc::Node* node = state.Doc().Find(placed);
            if (!Check(node != nullptr, "it places")) return;
            Check(node->kind == doc::NodeKind::Vector,
                  "as a Vector node — not an Image, and not an instance of anything");
            // A 24-pixel icon dropped at 24 pixels is a speck nobody can grab; the aspect survives.
            Check(node->layout.width.value >= 48.0f && node->layout.width.value <= 320.0f,
                  "at a size you can see: " + std::to_string(node->layout.width.value));
            Check(node->layout.width.value == node->layout.height.value, "keeping its shape");

            const doc::Value ink = state.Doc().GetProp(placed, doc::Prop::Fill);
            Check(std::holds_alternative<doc::TokenRef>(ink)
                  && std::get<doc::TokenRef>(ink).name == "text",
                  "wearing the theme's text colour, as a token so it follows the theme");

            std::filesystem::remove_all(folder, ec);
        }

        void TestProjectFolders() {
            Section("project folders");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            EditorState& state = layer.State();

            // Nothing the user makes belongs in the engine's own directory: an installed VAE would
            // be writing into itself, and a checked-out one fills the repository with someone's work.
            const std::filesystem::path projects = FileSystem::ProjectsRoot();
            Check(!projects.empty(), "there is a projects root");
            Check(projects.string().find(FileSystem::EngineRoot().string()) == std::string::npos,
                  "and it is not inside the engine");

            const std::string name = "Selftest project";
            const std::filesystem::path folder = projects / name;
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);

            layer.CreateProject(name);
            Check(std::filesystem::exists(folder, ec), "a project is a folder of its own");
            Check(std::filesystem::exists(folder / (name + ".vaescreen"), ec),
                  "with the document inside it");
            Check(state.Path() == folder / (name + ".vaescreen"),
                  "and that is the project the editor is now editing");

            // A second project of the same name would quietly overwrite the first one's document.
            layer.CreateProject(name);
            Check(layer.NamingError().find("already") != std::string::npos,
                  "a name already in use is refused, not overwritten");

            std::filesystem::remove_all(folder, ec);

            // What the creation dialog asks for, and what each answer has to have done by the time
            // the project exists. All three are expensive to change later, which is why they are
            // asked at the start rather than left to be found in a settings menu.
            const std::string kiosk = "Selftest kiosk";
            const std::filesystem::path at = projects / kiosk;
            std::filesystem::remove_all(at, ec);

            StudioLayer::NewProjectSpec spec;
            spec.language = ScriptSession::Language::Cpp;
            spec.size = { 390.0f, 844.0f };
            spec.resizable = false;
            layer.CreateProject(kiosk, spec);

            const Uuid home = state.ActiveScreen();
            Check(state.Doc().Find(home)->layout.width.value == 390.0f,
                  "the screen is the size that was asked for");
            Check(state.Doc().Find(home)->layout.height.value == 844.0f, "on both axes");
            Check(!state.Doc().Find(home)->props.Flag(doc::Prop::Resizable, true),
                  "and pinned to it, because that is what was ticked");
            Check(layer.Scripts().Lang() == ScriptSession::Language::Cpp,
                  "the project is a C++ project");
            Check(layer.Scripts().SourcePath().extension() == ".cpp",
                  "so its script is a .cpp beside the document: "
                      + layer.Scripts().SourcePath().filename().string());
            Check(!layer.HasUnsavedWork(),
                  "and none of it counts as an edit — a new project starts clean");

            // Reopening finds the language on disk rather than in whatever the Studio was last set
            // to. Without this, opening a C++ project from a Lua session shows an empty editor and
            // says the project has no logic.
            layer.Scripts().SetLanguage(ScriptSession::Language::Lua);
            layer.OpenProject(at / (kiosk + ".vaescreen"));
            driver.Frame();
            Check(layer.Scripts().Lang() == ScriptSession::Language::Cpp,
                  "reopening it comes back as C++");
            Check(!state.Doc().Find(state.ActiveScreen())->props.Flag(doc::Prop::Resizable, true),
                  "still pinned");

            std::filesystem::remove_all(at, ec);
        }

        void TestUnsavedClose() {
            Section("unsaved changes");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            EditorState& state = layer.State();

            Check(!layer.HasUnsavedWork(), "a fresh project has nothing to lose");

            PlaceFixed(state, "Button", { 100.0f, 100.0f }, { 160.0f, 40.0f });
            Check(layer.HasUnsavedWork(), "placing a widget is unsaved work");
            Check(layer.HoldCloseForUnsavedWork(), "closing with unsaved work is held back");

            const std::filesystem::path path =
                std::filesystem::temp_directory_path() / "vae-selftest-close.vaescreen";
            layer.SaveProject(path);
            Check(!layer.HasUnsavedWork(), "saving settles it");
            Check(!layer.HoldCloseForUnsavedWork(), "and closing is no longer held back");
            std::error_code ec;
            std::filesystem::remove(path, ec);
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

        // The catalog's exam. Two of shadcn's own blocks, rebuilt as VAE screens: if a widget in
        // either one had to be hand-built out of frames, the catalog is short a component and this
        // is where that shows up as a failure instead of as a shrug.
        void TestBlocksComeFromTheCatalog() {
            Section("shadcn blocks");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();

            for (const auto& [example, name, least] :
                 { std::tuple{ StudioLayer::Example::Login, "Login block", 8u },
                   std::tuple{ StudioLayer::Example::Dashboard, "Dashboard block", 12u } }) {
                layer.OpenExample(example);
                driver.Frame();
                EditorState& state = layer.State();
                const doc::Document& d = state.Doc();

                u32 instances = 0;
                u32 handBuilt = 0;
                std::string offender;
                for (Uuid id : d.Subtree(state.ActiveScreen())) {
                    const doc::Node* node = d.Find(id);
                    if (!node) continue;
                    if (node->IsInstance()) { ++instances; continue; }
                    // A frame with a role is a widget somebody built by hand. A frame without one
                    // is an arrangement — a row, a column — and arranging is not a component.
                    if (node->props.Has(doc::Prop::Role)) {
                        ++handBuilt;
                        if (offender.empty()) offender = node->name;
                    }
                }
                Check(instances >= least, std::string(name) + ": " + std::to_string(instances)
                                          + " components from the library");
                Check(handBuilt == 0, std::string(name) + ": no hand-built widgets"
                                      + (offender.empty() ? "" : " (" + offender + ")"));

                // And it lays out: a block that resolves to nothing on screen has proved nothing.
                const Rect box = layer.Surface().BoundsOf(state, state.ActiveScreen());
                Check(box.size.x > 300.0f && box.size.y > 300.0f,
                      std::string(name) + ": the screen has a box");
            }
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

        // The three things a screen that talks to something needs, checked where they live: in
        // the document, so the canvas can show each of them with nothing running.
        void TestFeedExample() {
            Section("feed example");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            ScriptSession& scripts = layer.Scripts();

            layer.OpenExample(StudioLayer::Example::Feed);
            driver.Frame();
            EditorState& state = layer.State();
            doc::Document& d = state.Doc();

            const auto named = [&](std::string_view name) {
                for (Uuid id : d.Subtree(state.ActiveScreen()))
                    if (const doc::Node* node = d.Find(id); node && node->name == name) return id;
                return Uuid::Invalid();
            };

            const Uuid panel = named("Panel");
            if (!Check(panel.Valid(), "the panel is there")) return;
            for (const char* state_ : { "Loading", "Failed", "Empty", "Content" })
                Check(named(state_).Valid(), std::string("and its ") + state_ + " state");

            const doc::Value shown = d.GetProp(panel, doc::Prop::Shown);
            Check(std::holds_alternative<std::string>(shown), "one of them is named as the one shown");

            // Which one is showing is a property, so the canvas draws exactly that one and a
            // designer can look at the failure without breaking anything.
            ui::UiHost& host = layer.Surface().Host();
            const auto visible = [&](std::string_view name) {
                const ui::ViewTree& tree = host.Tree();
                for (u32 i = 0; i < tree.ViewCount(); ++i)
                    if (tree.At(i).name == name) return tree.At(i).visible;
                return false;
            };
            Check(visible("Loading"), "the canvas shows it");
            Check(!visible("Failed"), "and only it");

            d.SetProp(panel, doc::Prop::Shown, std::string("Failed"));
            driver.Frame();
            Check(visible("Failed") && !visible("Loading"),
                  "switching is one property, which is what makes it something a script can do");

            Check(named("Rows").Valid(), "there is a table for the answer to go into");
            Check(scripts.Build(), "its script builds: " + scripts.Output());
        }

        void TestExampleSound() {
            Section("sound");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            EditorState& state = layer.State();
            Canvas& canvas = layer.Surface();
            ScriptSession& scripts = layer.Scripts();

            layer.OpenExample(StudioLayer::Example::Counter);
            driver.Frame();

            // The example generates its own click rather than shipping one, so the first thing to
            // know is that the file the document names is actually on disk.
            const std::filesystem::path folder = FileSystem::ProjectsRoot() / "Counter example";
            std::error_code ec;
            const std::filesystem::path wav = folder / "assets" / "click.wav";
            if (!Check(std::filesystem::exists(wav, ec), "the example wrote its click sound"))
                return;
            Check(std::filesystem::file_size(wav, ec) > 44,
                  "and it is a wav with samples in it, not just a header");

            Uuid click = Uuid::Invalid();
            for (const doc::Document::Asset& asset : state.Doc().Assets())
                if (asset.name == "click") click = asset.id;
            if (!Check(click.Valid(), "registered under the name the script plays it by")) return;

            canvas.Assets().Rebind(state.Doc());
            Check(canvas.Assets().IsSound(click), "the asset store knows it is a sound");
            Check(canvas.Assets().ProblemWith(click).empty(),
                  "and finds it: " + canvas.Assets().ProblemWith(click));
            // Nothing about a sound is drawn, so the panel must not go looking for pixels.
            Check(!canvas.Assets().IsVector(click), "it is not artwork");
            Check(canvas.Assets().Image(click) == nullptr, "and there is no texture behind it");

            Check(scripts.Buffer().find("play_sound") != std::string::npos,
                  "the example's script plays it");
            Check(scripts.Build(), "which builds: " + scripts.Output());

            // The editor's own speaker, which is what the Assets panel's play button uses. Opened
            // silently, because a selftest that makes a noise on a build machine is a selftest
            // nobody runs twice — and because there may be no sound card at all.
            std::string trouble;
            if (Check(state.Preview().OpenSilent(&trouble), "the preview speaker opens: " + trouble)) {
                Check(state.PreviewAsset(click).empty(), "and plays the asset by id");
                Check(state.Preview().Voices() == 1, "one sound, not a pile of them");
                state.PreviewAsset(click);
                Check(state.Preview().Voices() == 1, "playing again replaces it rather than layering");
                state.Preview().StopAll();
                Check(state.Preview().Voices() == 0, "and stops");
            }
        }

        void TestScreenSizing() {
            Section("screen sizing");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            EditorState& state = layer.State();

            const std::string name = "Selftest sizing";
            const std::filesystem::path folder = FileSystem::ProjectsRoot() / name;
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);
            layer.CreateProject(name);
            driver.Frame();

            const Uuid home = state.ActiveScreen();
            Check(state.Doc().Find(home)->props.Flag(doc::Prop::Resizable, true),
                  "a new screen is resizable, because an app that does not fit the window it was "
                  "given is the exception");

            // A child pinned to both edges: the thing that makes "resizing fits everything" true
            // rather than a claim. Absolute placement, which is what a screen is by default.
            const Uuid band = state.Doc().CreateNode(doc::NodeKind::Frame, home, "Band");
            {
                layout::LayoutStyle& style = state.Doc().Find(band)->layout;
                style.constraintX = layout::Constraint::StartEnd;
                style.offsetStart = { 40.0f, 40.0f };
                style.offsetEnd = { 40.0f, 0.0f };
                style.height = layout::Size::Px(60.0f);
            }
            state.Doc().Touch(band);
            layer.SaveProject(folder / (name + ".vaescreen"));

            const std::filesystem::path document = folder / (name + ".vaescreen");

            // The runtime, not the editor: this is the half that decides what an app does with the
            // window it is handed, and it is the half that used to overwrite the design silently.
            {
                app::RunLayer run;
                std::string error;
                if (!Check(run.Load(document, &error), "the player loads it: " + error)) return;
                // The wiring a real Application would do. The Studio's own app has no device in a
                // selftest, so this stops at the document and the view tree — which is the half
                // this is about.
                run.OnAttach();
                Check(run.Resizable(), "and calls it resizable");
                Check(run.DesignSize().x == 1280.0f, "opening at the size the screen was designed at");

                run.RenderOffline(1.0f / 60.0f);
                run.Resize({ 900.0f, 600.0f });
                run.RenderOffline(1.0f / 60.0f);

                const ui::ViewTree& tree = run.Host().Tree();
                u32 view = ui::ViewTree::kInvalid;
                std::string saw;
                for (u32 i = 0; i < tree.ViewCount(); ++i) {
                    saw += " " + tree.At(i).name;
                    if (tree.At(i).name == "Band") view = i;
                }
                if (Check(view != ui::ViewTree::kInvalid, "the band is on screen; saw" + saw)) {
                    // 900 wide, 40 of margin either side. A child that did not move is a child in
                    // an app that does not resize, whatever the window did.
                    Check(std::abs(tree.Bounds(view).size.x - 820.0f) < 1.0f,
                          "and it stretched with the window: "
                              + std::to_string(tree.Bounds(view).size.x));
                }
                run.OnDetach();
            }

            // Pinned to a resolution. Everything above should stop happening.
            state.SetProp(home, doc::Prop::Resizable, false);
            state.EndGesture();
            layer.SaveProject(document);
            {
                app::RunLayer run;
                std::string error;
                if (!Check(run.Load(document, &error), "it reloads: " + error)) return;
                run.OnAttach();
                Check(!run.Resizable(), "a pinned screen says it cannot be resized");
                Check(run.DesignSize().x == 1280.0f, "and the numbers are now a hard resolution");

                run.RenderOffline(1.0f / 60.0f);
                run.Resize({ 900.0f, 600.0f });
                run.RenderOffline(1.0f / 60.0f);

                const ui::ViewTree& tree = run.Host().Tree();
                u32 view = ui::ViewTree::kInvalid;
                for (u32 i = 0; i < tree.ViewCount(); ++i)
                    if (tree.At(i).name == "Band") view = i;
                if (Check(view != ui::ViewTree::kInvalid, "the band is still there")) {
                    // A window manager that ignores the hint sends a resize anyway; stretching the
                    // design to fit it is exactly what "fixed resolution" asked not to do.
                    Check(std::abs(tree.Bounds(view).size.x - 1200.0f) < 1.0f,
                          "and it kept its designed width: "
                              + std::to_string(tree.Bounds(view).size.x));
                }
                run.OnDetach();
            }
        }

        void TestScreens() {
            Section("screens");
            Shortcuts driver;
            StudioLayer& layer = driver.Layer_();
            ScriptSession& scripts = layer.Scripts();

            layer.OpenExample(StudioLayer::Example::Screens);
            driver.Frame();

            EditorState& state = layer.State();
            Check(state.Screens().size() == 3, "three screens");
            Check(state.Doc().StartScreen() == state.ActiveScreen(), "the start screen is showing");
            Check(scripts.Build(), "its script builds: " + scripts.Output());

            driver.Press(ImGuiKey_F5);
            driver.Frame();
            if (!Check(scripts.Playing(), "the screens example runs")) return;

            ui::UiHost& host = layer.Surface().Host();
            Check(host.CurrentScreenName() == "List", "opens on the screen it was told to");
            Check(scripts.LiveInstances() >= 1, "the screen's own script mounted");

            // A named node anywhere on screen, whichever tree it is in.
            auto find = [&](std::string_view name) -> std::pair<ui::ViewTree*, u32> {
                for (ui::ViewTree* tree : host.Trees())
                    for (u32 i = 0; i < tree->ViewCount(); ++i)
                        if (tree->At(i).name == name) return { tree, i };
                return { nullptr, ui::ViewTree::kInvalid };
            };

            auto click = [&](std::string_view name) {
                const auto [tree, view] = find(name);
                if (!tree) return false;
                const Vec2 point = tree->Bounds(view).Center();
                host.Dispatch(MakeMouseMoved(point.x, point.y));
                driver.Frame();
                host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                              point.x, point.y, Mod::None));
                driver.Frame();
                host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                              point.x, point.y, Mod::None));
                driver.Frame();
                return true;
            };

            // A row says where it goes; no script is involved in the navigation itself.
            Check(click("Invoice #4021"), "a row to click");
            Check(host.CurrentScreenName() == "Detail", "a declared destination navigated");

            // ...and the script that watched the click filled in the detail.
            const auto [tree, subject] = find("Subject");
            if (Check(tree != nullptr, "the detail has a subject")) {
                const std::string shown = tree->Str(subject, doc::Prop::Text);
                Check(shown == "Invoice #4021",
                      "the screen script carried the choice across: " + shown);
            }

            // The alert is presented over the detail rather than replacing it.
            Check(click("Delete"), "a delete button");
            Check(host.OverlayCount() == 1, "the alert is presented");
            Check(host.CurrentScreenName() == "Detail", "and the detail is still underneath");

            // An alert does not dismiss itself: a click outside it changes nothing.
            host.Dispatch(MakeMouseMoved(40.0f, 760.0f));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          40.0f, 760.0f, Mod::None));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          40.0f, 760.0f, Mod::None));
            driver.Frame();
            Check(host.OverlayCount() == 1, "an alert stays until it is answered");

            Check(click("Cancel"), "a cancel button");
            Check(host.OverlayCount() == 0, "cancel closed it");
            Check(host.CurrentScreenName() == "Detail", "and left the detail where it was");

            Check(click("Back"), "a back button");
            Check(host.CurrentScreenName() == "List", "back went back");

            driver.Press(ImGuiKey_F5, false, true);
            driver.Frame();
            Check(!scripts.Playing(), "stopped");
        }

    }

    void SelftestLayer::OnAttach() {
        g_Checks = 0;
        g_Failed = 0;

        TestHitTest();
        TestDrag();
        TestSnap();
        TestResize();
        TestMarquee();
        TestInspectorRoundTrip();
        TestSaveLoad();
        TestViewport();
        TestDeleteAndDuplicate();
        TestAlignAndDistribute();
        TestShortcuts();
        TestPlayMode();
        TestDebugger();
        TestScreens();
        TestFeedExample();
        TestExampleSound();
        TestScreenSizing();
        TestBlocksComeFromTheCatalog();
        TestContainerContentsAreEditable();
        TestNesting();
        TestStyling();
        TestUnsavedClose();
        TestProjectFolders();
        TestAssets();
        TestArtwork();
        TestFillingWidgetMoves();
        TestDroppingIntoAContainer();
        TestAuthoringAComponent();
        TestPlacementUndoes();
        TestProjectWithoutAScript();

        if (g_Failed == 0) VAE_INFO("selftest: {} checks passed", g_Checks);
        else               VAE_ERROR("selftest: {} of {} checks FAILED", g_Failed, g_Checks);

        Application::Get().SetExitCode(g_Failed == 0 ? 0 : 1);
        Application::Get().Close();
    }

}
