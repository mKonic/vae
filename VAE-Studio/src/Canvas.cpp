#include "Canvas.h"

#include "vae/app/ImGuiLayer.h"
#include "vae/base/Log.h"
#include "vae/core/Application.h"
#include "vae/text/FontDB.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace vae {

    namespace {
        constexpr f32 kMinZoom = 0.05f;
        constexpr f32 kMaxZoom = 32.0f;
        constexpr f32 kHandleRadius = 5.0f;      // screen px
        constexpr f32 kSnapDistance = 6.0f;      // screen px

        ImVec2 Im(Vec2 v) { return { v.x, v.y }; }
        Vec2   Vae(ImVec2 v) { return { v.x, v.y }; }

        ImU32 Col(Color c) {
            return IM_COL32(static_cast<int>(c.r * 255.0f), static_cast<int>(c.g * 255.0f),
                            static_cast<int>(c.b * 255.0f), static_cast<int>(c.a * 255.0f));
        }

        const ImU32 kAccent   = IM_COL32(93, 130, 228, 255);
        const ImU32 kAccentBg = IM_COL32(93, 130, 228, 40);
        const ImU32 kGuide    = IM_COL32(236, 72, 153, 220);
    }

    bool Canvas::Init(gpu::Device& device) {
        m_Device = &device;
        const gpu::Format format = gpu::Format::RGBA8_UNORM;
        if (!m_Renderer.Init(device, format)) {
            VAE_ERROR("canvas renderer failed to initialise");
            return false;
        }
        text::FontDB::Get().LoadDefaults();
        m_Atlas.Init(device);
        m_Assets.SetDevice(&device);
        return true;
    }

    void Canvas::Shutdown() {
        if (m_TargetHandle) app::ImGuiLayer::ReleaseTextureHandle(m_TargetHandle);
        m_TargetHandle = 0;
        m_Target.reset();
        m_Atlas.Shutdown();
        m_Renderer.Shutdown();
    }

    void Canvas::SetPreview(bool on) {
        m_Preview = on;
        m_Gesture = Gesture::None;
    }

    Vec2 Canvas::ToDocument(Vec2 screen) const {
        return m_Pan + (screen - m_ViewportPos) / m_Zoom;
    }

    Vec2 Canvas::ToScreen(Vec2 document) const {
        return m_ViewportPos + (document - m_Pan) * m_Zoom;
    }

    // ------------------------------------------------------------------------------ rendering

    void Canvas::EnsureTarget(gpu::Device& device) {
        const auto width  = static_cast<u32>(std::max(m_ViewportSize.x, 1.0f));
        const auto height = static_cast<u32>(std::max(m_ViewportSize.y, 1.0f));
        if (m_Target && m_Target->Width() == width && m_Target->Height() == height) return;

        // The descriptor set points at the old image view, so it has to go with it.
        if (m_TargetHandle) {
            device.WaitIdle();
            app::ImGuiLayer::ReleaseTextureHandle(m_TargetHandle);
            m_TargetHandle = 0;
        }

        gpu::RenderTargetDesc desc;
        desc.width = width;
        desc.height = height;
        desc.colorFormat = gpu::Format::RGBA8_UNORM;
        desc.debugName = "canvas";
        m_Target = device.CreateRenderTarget(desc);
        if (m_Target) m_TargetHandle = app::ImGuiLayer::TextureHandle(m_Target->ColorTexture());
    }

    void Canvas::DrawGrid(Vec2 viewport) {
        // An adaptive spacing: whichever power-of-two multiple of 8 document units lands closest to
        // a comfortable on-screen pitch. Without this the grid either disappears when you zoom out
        // or turns into a solid fill.
        f32 spacing = 8.0f;
        while (spacing * m_Zoom < 8.0f)  spacing *= 4.0f;
        while (spacing * m_Zoom > 96.0f) spacing *= 0.25f;

        const Vec2 topLeft = m_Pan;
        const Vec2 bottomRight = m_Pan + viewport / m_Zoom;
        const Color minor{ 1.0f, 1.0f, 1.0f, 0.045f };
        const Color major{ 1.0f, 1.0f, 1.0f, 0.09f };
        const f32 thickness = 1.0f / m_Zoom;   // one device pixel, whatever the zoom

        const f32 firstX = std::floor(topLeft.x / spacing) * spacing;
        for (f32 x = firstX; x <= bottomRight.x; x += spacing) {
            const bool strong = std::fmod(std::abs(x), spacing * 4.0f) < 0.01f;
            m_List.AddLine({ x, topLeft.y }, { x, bottomRight.y }, thickness, strong ? major : minor);
        }
        const f32 firstY = std::floor(topLeft.y / spacing) * spacing;
        for (f32 y = firstY; y <= bottomRight.y; y += spacing) {
            const bool strong = std::fmod(std::abs(y), spacing * 4.0f) < 0.01f;
            m_List.AddLine({ topLeft.x, y }, { bottomRight.x, y }, thickness, strong ? major : minor);
        }
    }

    Vec2 Canvas::DesignSize(EditorState& state) const {
        const doc::Node* root = state.Doc().Find(state.ActiveScreen());
        if (!root) return { 0.0f, 0.0f };
        Vec2 size{ root->layout.width.value, root->layout.height.value };
        // A component that hugs has no size of its own until something lays it out, so it is given
        // a page to sit on. Without this, opening one to edit it shows a zero-by-zero canvas.
        if (root->layout.width.mode  != layout::SizeMode::Fixed) size.x = 1280.0f;
        if (root->layout.height.mode != layout::SizeMode::Fixed) size.y = 800.0f;
        return size;
    }

    void Canvas::DrawScreenFrame(EditorState& state) {
        const doc::Node* screen = state.Doc().Find(state.ActiveScreen());
        if (!screen) return;
        const Rect frame{ { 0.0f, 0.0f }, m_LastScreenSize };

        m_List.AddShadow(frame, draw::ShadowSpec{ { 0.0f, 0.0f, 0.0f, 0.55f },
                                                  { 0.0f, 8.0f / m_Zoom }, 24.0f / m_Zoom, 0.0f });
        m_List.AddRect(frame, draw::Paint::Solid({ 0.0f, 0.0f, 0.0f, 0.0f }), {},
                       draw::Stroke{ 1.0f / m_Zoom, { 1.0f, 1.0f, 1.0f, 0.16f } });
    }

    void Canvas::ShowSampleRows(bool on) {
        m_SampleRows = on;
        m_Host.Tree().ShowSampleRows(on);
    }

    void Canvas::SyncScene(EditorState& state, f32 dt) {
        if (state.ActiveScreen() != m_BoundScreen) {
            // Framed after it has been laid out, not now: a component that hugs has no size until
            // the solver has run, and framing against the size the *previous* root had is how
            // opening one leaves it as a speck in the corner. Only on a *switch* — the first bind
            // is a fresh document, and the layer frames that one when it finishes opening it.
            m_PendingFrame = m_BoundScreen.Valid();
            m_BoundScreen = state.ActiveScreen();
            m_Host.SetDocument(state.Doc(), m_BoundScreen);
            m_Host.Tree().ShowSampleRows(m_SampleRows);
            m_Assets.Rebind(state.Doc());
        }

        const Vec2 available = DesignSize(state);
        m_Host.Update(available, dt);
        // What it came out as, not what it was offered: a hugging component is framed around
        // itself, and a screen with a stated size lays out to exactly that anyway.
        const Vec2 laid = m_Host.Tree().RootSize();
        m_LastScreenSize = { laid.x > 1.0f ? laid.x : available.x,
                             laid.y > 1.0f ? laid.y : available.y };

        if (m_PendingFrame && HasViewport()) {
            FrameAll(state);
            m_PendingFrame = false;
        }

        // In play mode the widgets' actions belong to the scripts, so they are taken rather than
        // dropped. In design mode nothing is listening and the queue would grow forever.
        if (m_Preview && m_Pump) m_Pump(dt);

        // A transition is the other thing that changes the screen without any input, so the idle
        // loop has to be held open for exactly as long as one is running.
        if (m_Host.Animating()) Application::Get().RequestFrame();
    }

    void Canvas::ResyncAfterNavigation() {
        m_Host.Update(m_LastScreenSize, 0.0f);
    }

    void Canvas::BuildScene(EditorState& state) {
        m_List.Reset();
        if (m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;

        SyncScene(state, ImGui::GetIO().DeltaTime);

        m_List.PushTransform({ m_Zoom, m_Zoom }, -m_Pan * m_Zoom);
        // Preview is meant to look like the app, not like the app inside an editor.
        if (!m_Preview) {
            DrawGrid(m_ViewportSize);
            DrawScreenFrame(state);
        }

        ui::PaintContext paint;
        paint.list = &m_List;
        paint.atlas = &m_Atlas;
        paint.assets = &m_Assets;
        m_Host.Paint(paint);
        m_Host.ClearActions();
        m_List.PopTransform();
    }

    void Canvas::OnRender(gpu::CommandList& cmd, EditorState& state) {
        if (!m_Device || m_ViewportSize.x < 1.0f) return;
        EnsureTarget(*m_Device);
        if (!m_Target) return;

        BuildScene(state);

        gpu::RenderPassDesc pass;
        pass.target = m_Target.get();
        pass.clearColor = { 0.055f, 0.06f, 0.075f, 1.0f };
        cmd.BeginRenderPass(pass);
        m_Renderer.NewFrame();
        m_Renderer.Render(cmd, m_List, m_ViewportSize);
        cmd.EndRenderPass();
    }

    // ------------------------------------------------------------------------------ queries

    Rect Canvas::NodeBounds(EditorState& state, Uuid id) const {
        (void)state;
        const ui::ViewTree& tree = m_Host.Tree();
        const u32 view = tree.ViewOf(ui::WidgetId{ id });
        if (view != ui::ViewTree::kInvalid) return tree.Bounds(view);

        // An instance is addressed by the instance id, and its view carries the component root as
        // its source, so the plain lookup above misses it.
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).instanceId == id) return tree.Bounds(i);
        return {};
    }

    Rect Canvas::SelectionBounds(EditorState& state) const {
        Rect bounds{};
        bool first = true;
        for (Uuid id : state.Selection()) {
            const Rect box = NodeBounds(state, id);
            if (box.size.x <= 0.0f && box.size.y <= 0.0f) continue;
            if (first) { bounds = box; first = false; continue; }
            bounds = Rect::FromEdges(std::min(bounds.Left(), box.Left()),
                                     std::min(bounds.Top(), box.Top()),
                                     std::max(bounds.Right(), box.Right()),
                                     std::max(bounds.Bottom(), box.Bottom()));
        }
        return bounds;
    }

    Uuid Canvas::DropTargetAt(EditorState& state, Vec2 document) const {
        for (Uuid at = PickAt(state, document); at.Valid(); ) {
            const doc::Node* node = state.Doc().Find(at);
            if (!node) break;
            if (node->IsInstance() && state.Doc().SlotOf(node->componentId).Valid()) return at;
            at = node->parent;
        }
        return state.ActiveScreen();
    }

    bool Canvas::Authored(EditorState& state, Uuid id) const {
        const Uuid root = m_Host.Tree().RootId();
        if (!root.Valid()) return false;
        for (Uuid at = id; at.Valid(); ) {
            if (at == root) return true;
            const doc::Node* node = state.Doc().Find(at);
            if (!node) break;
            at = node->parent;
        }
        return false;
    }

    Uuid Canvas::PickAt(EditorState& state, Vec2 document) const {
        const ui::ViewTree& tree = m_Host.Tree();
        const u32 view = tree.HitTest(document);
        if (view == ui::ViewTree::kInvalid) return Uuid::Invalid();
        if (view == tree.Root()) return Uuid::Invalid();     // the screen itself is not a selection

        // A click selects the innermost thing the page itself wrote down. A component's internals
        // are not that — clicking a card's title gives you the card, and reaching the title is the
        // deliberate act of opening the card, the same rule Figma follows.
        //
        // What a page put *into* an instance — the four cards inside a grid — is the page's own
        // writing, wherever it happens to appear on screen, so it picks as itself. Attributing it
        // to the instance around it would make everything in a container unreachable.
        for (u32 at = view; at != ui::ViewTree::kInvalid && at != tree.Root();
             at = tree.At(at).parent) {
            const Uuid authored = tree.At(at).authoredId;
            if (authored.Valid() && Authored(state, authored)) return authored;
        }
        return Uuid::Invalid();
    }

    int Canvas::PickHandle(EditorState& state, Vec2 screen) const {
        if (state.Selection().empty()) return HandleNone;
        const Rect bounds = SelectionBounds(const_cast<EditorState&>(state));
        if (bounds.size.x <= 0.0f || bounds.size.y <= 0.0f) return HandleNone;

        Vec2 points[HandleCount];
        bool live[HandleCount];
        HandlePoints(bounds, points, live);
        for (int i = 0; i < HandleCount; ++i) {
            if (!live[i]) continue;
            const Vec2 d = screen - points[i];
            if (std::abs(d.x) <= kHandleRadius + 2.0f && std::abs(d.y) <= kHandleRadius + 2.0f)
                return i;
        }
        return HandleNone;
    }

    // Handle placement, and which of them are worth offering. On a control only a few pixels tall
    // the edge handles cover the whole body, and dragging it anywhere becomes a resize — so below a
    // threshold only the corners survive, and they sit just outside the shape rather than on it.
    void Canvas::HandlePoints(const Rect& bounds, Vec2* points, bool* live) const {
        const Vec2 tl = ToScreen(bounds.pos);
        const Vec2 br = ToScreen({ bounds.Right(), bounds.Bottom() });
        const f32 width = br.x - tl.x;
        const f32 height = br.y - tl.y;
        const f32 out = kHandleRadius + 1.0f;

        const bool wide = width  >= kHandleRadius * 6.0f;
        const bool tall = height >= kHandleRadius * 6.0f;
        // Corners move outward when the shape is too small to host them inside.
        const f32 dx = wide ? 0.0f : out;
        const f32 dy = tall ? 0.0f : out;

        const Vec2 mid{ (tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f };
        points[TopLeft]     = { tl.x - dx, tl.y - dy };
        points[Top]         = { mid.x,     tl.y };
        points[TopRight]    = { br.x + dx, tl.y - dy };
        points[Right]       = { br.x,      mid.y };
        points[BottomRight] = { br.x + dx, br.y + dy };
        points[Bottom]      = { mid.x,     br.y };
        points[BottomLeft]  = { tl.x - dx, br.y + dy };
        points[Left]        = { tl.x,      mid.y };

        for (int i = 0; i < HandleCount; ++i) live[i] = true;
        live[Top] = live[Bottom] = tall;
        live[Left] = live[Right] = wide;
    }

    // ------------------------------------------------------------------------------ manipulation

    Vec2 Canvas::SnapMove(EditorState& state, Vec2 proposed, Vec2 size) {
        m_GuideRects.clear();
        const f32 threshold = kSnapDistance / m_Zoom;

        const doc::Node* screen = state.Doc().Find(m_BoundScreen);
        if (!screen) return proposed;
        const Vec2 screenSize = m_LastScreenSize;

        // Candidate lines: the screen's edges and centre, plus every sibling that is not being
        // dragged. Snapping to a node you are moving would fight the drag.
        std::vector<f32> xs{ 0.0f, screenSize.x * 0.5f, screenSize.x };
        std::vector<f32> ys{ 0.0f, screenSize.y * 0.5f, screenSize.y };
        for (Uuid sibling : screen->children) {
            if (state.IsSelected(sibling)) continue;
            const Rect box = NodeBounds(state, sibling);
            if (box.size.x <= 0.0f) continue;
            xs.insert(xs.end(), { box.Left(), box.Center().x, box.Right() });
            ys.insert(ys.end(), { box.Top(), box.Center().y, box.Bottom() });
        }

        auto snapAxis = [&](f32 value, f32 extent, const std::vector<f32>& lines, bool vertical) {
            const f32 edges[3] = { value, value + extent * 0.5f, value + extent };
            f32 best = threshold;
            f32 shift = 0.0f;
            f32 hitLine = 0.0f;
            for (f32 line : lines) {
                for (f32 edge : edges) {
                    const f32 distance = std::abs(edge - line);
                    if (distance >= best) continue;
                    best = distance;
                    shift = line - edge;
                    hitLine = line;
                }
            }
            if (best >= threshold) return value;
            // A guide is drawn the full length of the screen, which is what makes an alignment
            // readable at a glance rather than a number in a corner.
            m_GuideRects.push_back(vertical ? Rect{ { hitLine, 0.0f }, { 0.0f, screenSize.y } }
                                            : Rect{ { 0.0f, hitLine }, { screenSize.x, 0.0f } });
            return value + shift;
        };

        proposed.x = snapAxis(proposed.x, size.x, xs, true);
        proposed.y = snapAxis(proposed.y, size.y, ys, false);
        return proposed;
    }

    // A node that fills its parent, or is pinned to both its edges, has no position of its own to
    // change: dragging it moves its start and the solver puts the far edge straight back, which
    // reads as a resize that will not let go of the right-hand edge. Moving one is a statement that
    // it is somewhere in particular, so it gets a size and a single pin first.
    void Canvas::FreezeForMove(EditorState& state) {
        for (Uuid id : state.Selection()) {
            const doc::Node* node = state.Doc().Find(id);
            if (!node) continue;
            // Inside a stack the parent decides, and nothing here is being dragged anyway.
            const doc::Node* parent = state.Doc().Find(node->parent);
            if (parent && parent->layout.mode != layout::LayoutMode::Absolute) continue;

            const Rect box = NodeBounds(state, id);
            if (box.size.x <= 0.0f || box.size.y <= 0.0f) continue;

            layout::LayoutStyle style = node->layout;
            bool changed = false;
            const auto pinned = [](layout::Constraint c) {
                return c == layout::Constraint::End || c == layout::Constraint::StartEnd
                    || c == layout::Constraint::Scale;
            };
            const auto elastic = [](layout::SizeMode mode) {
                return mode == layout::SizeMode::Fill || mode == layout::SizeMode::Percent;
            };

            if (elastic(style.width.mode))  { style.width  = layout::Size::Px(box.size.x); changed = true; }
            if (elastic(style.height.mode)) { style.height = layout::Size::Px(box.size.y); changed = true; }
            if (pinned(style.constraintX)) { style.constraintX = layout::Constraint::Start; changed = true; }
            if (pinned(style.constraintY)) { style.constraintY = layout::Constraint::Start; changed = true; }
            if (changed) {
                style.offsetStart = box.pos;
                state.SetLayout(id, style);
            }
        }
    }

    // What the pointer is about to do, said in the shape of the pointer. The canvas is the one place
    // in the editor where a click means something different every few pixels, so guessing is costly.
    void Canvas::UpdateCursor(EditorState& state, Vec2 screen) {
        static constexpr ImGuiMouseCursor kForHandle[HandleCount] = {
            ImGuiMouseCursor_ResizeNWSE,   // TopLeft
            ImGuiMouseCursor_ResizeNS,     // Top
            ImGuiMouseCursor_ResizeNESW,   // TopRight
            ImGuiMouseCursor_ResizeEW,     // Right
            ImGuiMouseCursor_ResizeNWSE,   // BottomRight
            ImGuiMouseCursor_ResizeNS,     // Bottom
            ImGuiMouseCursor_ResizeNESW,   // BottomLeft
            ImGuiMouseCursor_ResizeEW,     // Left
        };

        if (m_Gesture == Gesture::Pan) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); return; }
        if (m_Gesture == Gesture::Move) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); return; }
        if (m_Gesture == Gesture::Resize && m_Handle != HandleNone) {
            ImGui::SetMouseCursor(kForHandle[m_Handle]);
            return;
        }
        if (!m_Hovered || m_Gesture != Gesture::None) return;

        // Space is the pan modifier, and says so before the drag rather than after it.
        if (ImGui::IsKeyDown(ImGuiKey_Space)) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); return; }

        const int handle = PickHandle(state, screen);
        if (handle != HandleNone) { ImGui::SetMouseCursor(kForHandle[handle]); return; }
        if (PickAt(state, ToDocument(screen)).Valid())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    void Canvas::ApplyMove(EditorState& state, Vec2 mouseDoc) {
        const Vec2 rawDelta = mouseDoc - m_GestureStart;
        const Vec2 snapped = SnapMove(state, m_GestureBounds.pos + rawDelta, m_GestureBounds.size);
        const Vec2 delta = snapped - m_GestureBounds.pos;

        for (std::size_t i = 0; i < state.Selection().size(); ++i) {
            const Uuid id = state.Selection()[i];
            if (i >= m_GestureStyles.size()) break;
            layout::LayoutStyle style = m_GestureStyles[i];
            style.offsetStart += delta;
            state.SetLayout(id, style);
        }
    }

    void Canvas::ApplyResize(EditorState& state, Vec2 mouseDoc) {
        if (state.Selection().empty() || m_GestureStyles.empty()) return;

        Rect box = m_GestureBounds;
        const bool left  = m_Handle == TopLeft || m_Handle == Left || m_Handle == BottomLeft;
        const bool right = m_Handle == TopRight || m_Handle == Right || m_Handle == BottomRight;
        const bool top    = m_Handle == TopLeft || m_Handle == Top || m_Handle == TopRight;
        const bool bottom = m_Handle == BottomLeft || m_Handle == Bottom || m_Handle == BottomRight;

        f32 l = box.Left(), t = box.Top(), r = box.Right(), b = box.Bottom();
        if (left)   l = std::min(mouseDoc.x, r - 1.0f);
        if (right)  r = std::max(mouseDoc.x, l + 1.0f);
        if (top)    t = std::min(mouseDoc.y, b - 1.0f);
        if (bottom) b = std::max(mouseDoc.y, t + 1.0f);
        box = Rect::FromEdges(l, t, r, b);

        // Resizing writes explicit pixel sizes: dragging a handle is a statement that this node is
        // exactly this big, which is precisely what Hug and Fill are not.
        const Uuid id = state.Selection().front();
        layout::LayoutStyle style = m_GestureStyles.front();
        style.offsetStart = box.pos;
        style.width  = layout::Size::Px(box.size.x);
        style.height = layout::Size::Px(box.size.y);
        state.SetLayout(id, style);
    }

    // Where a child's offsetStart is measured from, worked out from a child that is already
    // there: origin = (where it is) - (what its offset says). Exact, and it does not have to know
    // whether offsets are measured from the border box or the padded one.
    Vec2 Canvas::ContentOrigin(EditorState& state, Uuid parent, Uuid reference) const {
        if (const doc::Node* node = state.Doc().Find(reference))
            return NodeBounds(state, reference).pos - node->layout.offsetStart;
        return NodeBounds(state, parent).pos;
    }

    // Everything selected has to end up in one frame, so everything selected has to have come out
    // of one frame. Grouping across parents would have to decide which parent wins and move nodes
    // out of layouts they were positioned by; refusing is the honest answer.
    bool Canvas::CanGroup(EditorState& state) const {
        const std::vector<Uuid>& selection = state.Selection();
        if (selection.empty()) return false;
        Uuid parent = Uuid::Invalid();
        for (Uuid id : selection) {
            const doc::Node* node = state.Doc().Find(id);
            if (!node || node->kind == doc::NodeKind::Screen || !node->parent.Valid()) return false;
            if (!parent.Valid()) parent = node->parent;
            else if (node->parent != parent) return false;
        }
        return true;
    }

    void Canvas::GroupSelection(EditorState& state) {
        if (!CanGroup(state)) return;
        const std::vector<Uuid> selection = state.Selection();
        doc::Document& d = state.Doc();
        const Uuid parent = d.Find(selection.front())->parent;

        // Where the group lands: in front of the earliest of its members, so grouping does not
        // change what draws over what.
        u32 index = UINT32_MAX;
        for (Uuid id : selection) index = std::min(index, d.IndexInParent(id));

        const doc::Node* parentNode = d.Find(parent);
        const bool stacked = parentNode && parentNode->layout.mode == layout::LayoutMode::Stack;
        const Rect bounds = SelectionBounds(state);

        state.Commands().BeginTransaction("Group");

        auto create = CreateScope<doc::CreateNodeCommand>(doc::NodeKind::Frame, parent, "Group");
        doc::CreateNodeCommand* raw = create.get();
        state.Execute(std::move(create));
        const Uuid group = raw->Created();
        if (!group.Valid()) { state.Commands().EndTransaction(d); return; }

        layout::LayoutStyle style;
        if (stacked) {
            // Inside a stack, position is the stack's business. The group takes the parent's axis
            // and hugs, so the members keep their order and their spacing.
            style.mode = layout::LayoutMode::Stack;
            style.axis = parentNode->layout.axis;
            style.gap = parentNode->layout.gap;
            style.width = layout::Size::Hug();
            style.height = layout::Size::Hug();
        } else {
            // Absolute: the group is the box the selection occupied, and its members move into it.
            style.mode = layout::LayoutMode::Absolute;
            style.offsetStart = bounds.pos - ContentOrigin(state, parent, selection.front());
            style.width = layout::Size::Px(bounds.size.x);
            style.height = layout::Size::Px(bounds.size.y);
        }
        state.SetLayout(group, style);
        state.Execute(CreateScope<doc::ReparentCommand>(group, parent, index));

        for (Uuid id : selection) {
            const doc::Node* node = d.Find(id);
            if (!node) continue;
            layout::LayoutStyle child = node->layout;
            if (!stacked) child.offsetStart = NodeBounds(state, id).pos - bounds.pos;
            state.Execute(CreateScope<doc::ReparentCommand>(id, group, UINT32_MAX));
            state.SetLayout(id, child);
        }

        state.Commands().EndTransaction(d);
        state.Commands().Break();
        state.Select(group);
    }

    bool Canvas::CanUngroup(EditorState& state) const {
        if (state.Selection().size() != 1) return false;
        const doc::Node* node = state.Doc().Find(state.Selection().front());
        // A component master or an instance is not a bag of nodes somebody grouped; dissolving one
        // would be editing the component through the back door.
        return node && node->kind == doc::NodeKind::Frame && !node->children.empty()
            && !node->IsComponent() && !node->IsInstance() && node->parent.Valid();
    }

    void Canvas::UngroupSelection(EditorState& state) {
        if (!CanUngroup(state)) return;
        doc::Document& d = state.Doc();
        const Uuid group = state.Selection().front();
        const Uuid parent = d.Find(group)->parent;
        const u32 index = d.IndexInParent(group);
        const std::vector<Uuid> children = d.Find(group)->children;

        const doc::Node* parentNode = d.Find(parent);
        const bool stacked = parentNode && parentNode->layout.mode == layout::LayoutMode::Stack;
        const Vec2 origin = ContentOrigin(state, parent, group);

        state.Commands().BeginTransaction("Ungroup");
        u32 at = index;
        for (Uuid id : children) {
            const doc::Node* node = d.Find(id);
            if (!node) continue;
            // Measured before the move, because moving is what changes it.
            const Vec2 where = NodeBounds(state, id).pos;
            layout::LayoutStyle style = node->layout;
            if (!stacked) style.offsetStart = where - origin;
            state.Execute(CreateScope<doc::ReparentCommand>(id, parent, at++));
            state.SetLayout(id, style);
        }
        state.Execute(CreateScope<doc::DeleteNodeCommand>(group));
        state.Commands().EndTransaction(d);
        state.Commands().Break();
        state.SelectMany(children);
    }

    void Canvas::AlignSelection(EditorState& state, Edge edge) {
        const std::vector<Uuid> selection = state.Selection();
        if (selection.empty()) return;

        Rect frame = SelectionBounds(state);
        if (selection.size() < 2) {
            const doc::Node* screen = state.Doc().Find(m_BoundScreen);
            if (!screen) return;
            frame = Rect{ { 0.0f, 0.0f },
                          { screen->layout.width.value, screen->layout.height.value } };
        }

        state.Commands().BeginTransaction("Align");
        for (Uuid id : selection) {
            const doc::Node* node = state.Doc().Find(id);
            if (!node) continue;
            const Rect box = NodeBounds(state, id);
            if (box.size.x <= 0.0f && box.size.y <= 0.0f) continue;

            // Offsets are relative to the parent, bounds are absolute — so what is applied is the
            // delta, which is the same number in either space.
            layout::LayoutStyle style = node->layout;
            switch (edge) {
                case Edge::Left:    style.offsetStart.x += frame.Left() - box.Left(); break;
                case Edge::CentreX: style.offsetStart.x += frame.Center().x - box.Center().x; break;
                case Edge::Right:   style.offsetStart.x += frame.Right() - box.Right(); break;
                case Edge::Top:     style.offsetStart.y += frame.Top() - box.Top(); break;
                case Edge::CentreY: style.offsetStart.y += frame.Center().y - box.Center().y; break;
                case Edge::Bottom:  style.offsetStart.y += frame.Bottom() - box.Bottom(); break;
            }
            state.SetLayout(id, style);
        }
        state.Commands().EndTransaction(state.Doc());
        state.EndGesture();
    }

    void Canvas::DistributeSelection(EditorState& state, bool horizontal) {
        struct Item { Uuid id; Rect box; };
        std::vector<Item> items;
        for (Uuid id : state.Selection()) {
            const Rect box = NodeBounds(state, id);
            if (box.size.x <= 0.0f && box.size.y <= 0.0f) continue;
            items.push_back({ id, box });
        }
        // Two things are already distributed; three is the first case with an answer.
        if (items.size() < 3) return;

        std::sort(items.begin(), items.end(), [horizontal](const Item& a, const Item& b) {
            return horizontal ? a.box.Center().x < b.box.Center().x
                              : a.box.Center().y < b.box.Center().y;
        });

        // Equal gaps between the outermost two, which leaves the ends where they were put.
        const f32 span = horizontal ? items.back().box.Right() - items.front().box.Left()
                                    : items.back().box.Bottom() - items.front().box.Top();
        f32 used = 0.0f;
        for (const Item& item : items) used += horizontal ? item.box.size.x : item.box.size.y;
        const f32 gap = (span - used) / static_cast<f32>(items.size() - 1);

        state.Commands().BeginTransaction("Distribute");
        f32 cursor = horizontal ? items.front().box.Left() : items.front().box.Top();
        for (const Item& item : items) {
            const doc::Node* node = state.Doc().Find(item.id);
            if (!node) continue;
            layout::LayoutStyle style = node->layout;
            if (horizontal) style.offsetStart.x += cursor - item.box.Left();
            else            style.offsetStart.y += cursor - item.box.Top();
            state.SetLayout(item.id, style);
            cursor += (horizontal ? item.box.size.x : item.box.size.y) + gap;
        }
        state.Commands().EndTransaction(state.Doc());
        state.EndGesture();
    }

    // ------------------------------------------------------------------------------ input

    namespace {
        // Only the keys a widget actually reads. Printable characters arrive as text events, so
        // this table is navigation and editing, not a keyboard map.
        struct KeyPair { ImGuiKey imgui; Key key; };
        constexpr KeyPair kKeys[] = {
            { ImGuiKey_Tab, Key::Tab },             { ImGuiKey_Enter, Key::Enter },
            { ImGuiKey_KeypadEnter, Key::KPEnter }, { ImGuiKey_Escape, Key::Escape },
            { ImGuiKey_Backspace, Key::Backspace }, { ImGuiKey_Delete, Key::Delete },
            { ImGuiKey_LeftArrow, Key::Left },      { ImGuiKey_RightArrow, Key::Right },
            { ImGuiKey_UpArrow, Key::Up },          { ImGuiKey_DownArrow, Key::Down },
            { ImGuiKey_Home, Key::Home },           { ImGuiKey_End, Key::End },
            { ImGuiKey_PageUp, Key::PageUp },       { ImGuiKey_PageDown, Key::PageDown },
            { ImGuiKey_Space, Key::Space },         { ImGuiKey_A, Key::A },
            { ImGuiKey_C, Key::C },                 { ImGuiKey_V, Key::V },
            { ImGuiKey_X, Key::X },
        };

        u16 ModsNow() {
            const ImGuiIO& io = ImGui::GetIO();
            u16 mods = Mod::None;
            if (io.KeyShift) mods |= Mod::Shift;
            if (io.KeyCtrl)  mods |= Mod::Control;
            if (io.KeyAlt)   mods |= Mod::Alt;
            if (io.KeySuper) mods |= Mod::Super;
            return mods;
        }
    }

    // Play mode input. The canvas stops interpreting the pointer as a selection tool and starts
    // handing it to the runtime exactly the way the desktop backend does — same Event type, same
    // coordinates, so a widget cannot tell the difference between this and the shipped app.
    void Canvas::ForwardInput() {
        ImGuiIO& io = ImGui::GetIO();
        const Vec2 point = ToDocument(Vae(io.MousePos));
        const u16 mods = ModsNow();

        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
            m_Host.Dispatch(MakeMouseMoved(point.x, point.y));

        constexpr Mouse kButtons[] = { Mouse::Left, Mouse::Right, Mouse::Middle };
        for (int button = 0; button < 3; ++button) {
            if (m_Hovered && ImGui::IsMouseClicked(button))
                m_Host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, kButtons[button],
                                                point.x, point.y, mods));
            // A release is delivered wherever it happens: a drag that ends outside the widget
            // still has to end.
            if (ImGui::IsMouseReleased(button))
                m_Host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, kButtons[button],
                                                point.x, point.y, mods));
        }

        if (m_Hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
            m_Host.Dispatch(MakeScroll(io.MouseWheelH, io.MouseWheel, mods));

        for (const KeyPair& pair : kKeys) {
            if (ImGui::IsKeyPressed(pair.imgui, true))
                m_Host.Dispatch(MakeKey(EventType::KeyPressed, pair.key, mods,
                                        !ImGui::IsKeyPressed(pair.imgui, false)));
            if (ImGui::IsKeyReleased(pair.imgui))
                m_Host.Dispatch(MakeKey(EventType::KeyReleased, pair.key, mods));
        }

        for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
            m_Host.Dispatch(MakeTextInput(static_cast<u32>(io.InputQueueCharacters[i])));
    }

    void Canvas::HandleInput(EditorState& state) {
        ImGuiIO& io = ImGui::GetIO();
        const Vec2 mouse = Vae(io.MousePos);
        const Vec2 mouseDoc = ToDocument(mouse);

        // Zoom about the cursor, so the point under the pointer stays put.
        if (m_Hovered && io.MouseWheel != 0.0f) {
            const f32 factor = std::pow(1.15f, io.MouseWheel);
            const Vec2 anchor = mouseDoc;
            m_Zoom = std::clamp(m_Zoom * factor, kMinZoom, kMaxZoom);
            m_Pan = anchor - (mouse - m_ViewportPos) / m_Zoom;
        }

        // Middle-drag pans, and so does space-drag — the wheel is not always available, and a
        // trackpad has no middle button.
        const bool panning = ImGui::IsMouseDragging(ImGuiMouseButton_Middle)
                          || (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
        if (m_Hovered && panning) {
            m_Pan -= Vae(io.MouseDelta) / m_Zoom;
            m_Gesture = Gesture::Pan;
            return;
        }

        if (m_Preview) { ForwardInput(); return; }

        UpdateCursor(state, mouse);

        if (m_Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
            const int handle = PickHandle(state, mouse);
            if (handle != HandleNone) {
                m_Gesture = Gesture::Resize;
                m_Handle = handle;
                m_GestureStart = mouseDoc;
                m_GestureBounds = SelectionBounds(state);
                m_GestureStyles.clear();
                for (Uuid id : state.Selection())
                    if (const doc::Node* node = state.Doc().Find(id))
                        m_GestureStyles.push_back(node->layout);
                return;
            }

            const Uuid hit = PickAt(state, mouseDoc);
            const bool additive = io.KeyShift;
            // Double-clicking a label edits it, which is what every design tool does and what a
            // designer tries before looking for the Inspector.
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit.Valid()) {
                const doc::Node* node = state.Doc().Find(hit);
                if (node && node->kind == doc::NodeKind::Text) {
                    BeginTextEdit(state, hit);
                    return;
                }
            }
            if (hit.Valid()) {
                if (!state.IsSelected(hit)) state.Select(hit, additive);
                m_Gesture = Gesture::Move;
                m_GestureStart = mouseDoc;
                FreezeForMove(state);
                m_GestureBounds = SelectionBounds(state);
                m_GestureStyles.clear();
                for (Uuid id : state.Selection())
                    if (const doc::Node* node = state.Doc().Find(id))
                        m_GestureStyles.push_back(node->layout);
            } else {
                if (!additive) state.ClearSelection();
                m_Gesture = Gesture::Marquee;
                m_MarqueeStart = mouseDoc;
            }
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (m_Gesture == Gesture::Move) ApplyMove(state, mouseDoc);
            else if (m_Gesture == Gesture::Resize) ApplyResize(state, mouseDoc);
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (m_Gesture == Gesture::Marquee) {
                const Rect box = Rect::FromEdges(std::min(m_MarqueeStart.x, mouseDoc.x),
                                                 std::min(m_MarqueeStart.y, mouseDoc.y),
                                                 std::max(m_MarqueeStart.x, mouseDoc.x),
                                                 std::max(m_MarqueeStart.y, mouseDoc.y));
                std::vector<Uuid> hits;
                if (const doc::Node* screen = state.Doc().Find(m_BoundScreen)) {
                    for (Uuid child : screen->children) {
                        const Rect bounds = NodeBounds(state, child);
                        if (bounds.size.x <= 0.0f) continue;
                        if (!bounds.Intersect(box).Empty()) hits.push_back(child);
                    }
                }
                if (!hits.empty()) state.SelectMany(std::move(hits));
            }
            // One drag is one undo entry: the coalescing run ends here, not on the next command.
            if (m_Gesture == Gesture::Move || m_Gesture == Gesture::Resize) state.EndGesture();
            m_Gesture = Gesture::None;
            m_Handle = HandleNone;
            m_GuideRects.clear();
        }
    }

    // ------------------------------------------------------------------------------ overlay

    // Rulers, drawn over the canvas rather than beside it: they are a readout, and taking a strip
    // of the viewport away from the design permanently costs more than it gives.
    void Canvas::DrawRulers(EditorState& state) {
        constexpr f32 kThickness = 18.0f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const Vec2 tl = m_ViewportPos;
        const Vec2 br = m_ViewportPos + m_ViewportSize;

        const ImU32 face  = IM_COL32(22, 24, 31, 235);
        const ImU32 line  = IM_COL32(255, 255, 255, 40);
        const ImU32 label = IM_COL32(150, 156, 172, 255);

        draw->AddRectFilled(Im(tl), ImVec2(br.x, tl.y + kThickness), face);
        draw->AddRectFilled(Im(tl), ImVec2(tl.x + kThickness, br.y), face);

        // The selection's extent, so the numbers on the ruler answer "how wide is this".
        const Rect selection = SelectionBounds(state);
        if (selection.size.x > 0.0f || selection.size.y > 0.0f) {
            const Vec2 a = ToScreen(selection.pos);
            const Vec2 b = ToScreen({ selection.Right(), selection.Bottom() });
            draw->AddRectFilled(ImVec2(a.x, tl.y), ImVec2(b.x, tl.y + kThickness), kAccentBg);
            draw->AddRectFilled(ImVec2(tl.x, a.y), ImVec2(tl.x + kThickness, b.y), kAccentBg);
        }

        // A round step that never lets two labels collide, whatever the zoom.
        f32 step = std::pow(10.0f, std::floor(std::log10(std::max(64.0f / m_Zoom, 1.0f))));
        while (step * m_Zoom < 64.0f) step *= 2.0f;

        const Vec2 first{ std::floor(m_Pan.x / step) * step, std::floor(m_Pan.y / step) * step };
        const Vec2 last = m_Pan + m_ViewportSize / m_Zoom;
        char text[32];

        for (f32 x = first.x; x <= last.x; x += step) {
            const f32 sx = ToScreen({ x, 0.0f }).x;
            if (sx < tl.x + kThickness) continue;
            draw->AddLine(ImVec2(sx, tl.y + kThickness - 5.0f), ImVec2(sx, tl.y + kThickness), line);
            std::snprintf(text, sizeof text, "%g", x);
            draw->AddText(ImVec2(sx + 3.0f, tl.y + 2.0f), label, text);
        }
        for (f32 y = first.y; y <= last.y; y += step) {
            const f32 sy = ToScreen({ 0.0f, y }).y;
            if (sy < tl.y + kThickness) continue;
            draw->AddLine(ImVec2(tl.x + kThickness - 5.0f, sy), ImVec2(tl.x + kThickness, sy), line);
            std::snprintf(text, sizeof text, "%g", y);
            // Vertical labels are unreadable rotated and unreadable truncated, so they sit just
            // under their tick instead of on it.
            draw->AddText(ImVec2(tl.x + 2.0f, sy + 2.0f), label, text);
        }

        draw->AddRectFilled(Im(tl), ImVec2(tl.x + kThickness, tl.y + kThickness), face);
        draw->AddLine(ImVec2(tl.x, tl.y + kThickness), ImVec2(br.x, tl.y + kThickness), line);
        draw->AddLine(ImVec2(tl.x + kThickness, tl.y), ImVec2(tl.x + kThickness, br.y), line);
    }

    void Canvas::DrawOverlay(EditorState& state) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(Im(m_ViewportPos), Im(m_ViewportPos + m_ViewportSize), true);

        // Preview letterboxes: everything outside the screen is masked out, so what is left is the
        // app at its own size rather than a design floating on a canvas.
        if (m_Preview) {
            if (const doc::Node* screen = state.Doc().Find(m_BoundScreen)) {
                const Vec2 a = ToScreen({ 0.0f, 0.0f });
                const Vec2 b = ToScreen({ screen->layout.width.value, screen->layout.height.value });
                const Vec2 p0 = m_ViewportPos;
                const Vec2 p1 = m_ViewportPos + m_ViewportSize;
                const ImU32 mask = IM_COL32(9, 10, 13, 255);
                draw->AddRectFilled(Im(p0), ImVec2(p1.x, a.y), mask);
                draw->AddRectFilled(ImVec2(p0.x, b.y), Im(p1), mask);
                draw->AddRectFilled(ImVec2(p0.x, a.y), ImVec2(a.x, b.y), mask);
                draw->AddRectFilled(ImVec2(b.x, a.y), ImVec2(p1.x, b.y), mask);
            }
            draw->PopClipRect();
            return;
        }

        // Hover outline, so it is obvious what a click would take before it is taken.
        if (m_Hovered && m_Gesture == Gesture::None && !m_Preview) {
            const Uuid hover = PickAt(state, ToDocument(Vae(ImGui::GetIO().MousePos)));
            if (hover.Valid() && !state.IsSelected(hover)) {
                const Rect box = NodeBounds(state, hover);
                draw->AddRect(Im(ToScreen(box.pos)), Im(ToScreen({ box.Right(), box.Bottom() })),
                              IM_COL32(93, 130, 228, 140), 0.0f, 0, 1.0f);
            }
        }

        for (Uuid id : state.Selection()) {
            const Rect box = NodeBounds(state, id);
            if (box.size.x <= 0.0f && box.size.y <= 0.0f) continue;
            draw->AddRect(Im(ToScreen(box.pos)), Im(ToScreen({ box.Right(), box.Bottom() })),
                          kAccent, 0.0f, 0, 1.5f);
        }

        // Smart guides, drawn while a drag is snapped.
        for (const Rect& guide : m_GuideRects) {
            const Vec2 a = ToScreen(guide.pos);
            const Vec2 b = ToScreen({ guide.Right(), guide.Bottom() });
            draw->AddLine(Im(a), Im(b), kGuide, 1.0f);
        }

        const Rect bounds = SelectionBounds(state);
        if (!state.Selection().empty() && bounds.size.x > 0.0f && !m_Preview) {
            const Vec2 tl = ToScreen(bounds.pos);
            const Vec2 br = ToScreen({ bounds.Right(), bounds.Bottom() });

            // The box the handles belong to. With one node selected its own outline is that box, but
            // with several the handles would otherwise float in space with nothing joining them —
            // and a handle you cannot see the extent of reads as a mystery, not as a selection.
            if (state.Selection().size() > 1)
                draw->AddRect(Im(tl), Im(br), IM_COL32(93, 130, 228, 150), 0.0f, 0, 1.0f);

            Vec2 points[HandleCount];
            bool live[HandleCount];
            HandlePoints(bounds, points, live);
            for (int i = 0; i < HandleCount; ++i) {
                if (!live[i]) continue;
                const Vec2 p = points[i];
                draw->AddRectFilled(Im(p - Vec2{ kHandleRadius, kHandleRadius }),
                                    Im(p + Vec2{ kHandleRadius, kHandleRadius }),
                                    IM_COL32(255, 255, 255, 255), 1.0f);
                draw->AddRect(Im(p - Vec2{ kHandleRadius, kHandleRadius }),
                              Im(p + Vec2{ kHandleRadius, kHandleRadius }), kAccent, 1.0f, 0, 1.0f);
            }

            char label[64];
            std::snprintf(label, sizeof label, "%.0f x %.0f", bounds.size.x, bounds.size.y);
            draw->AddText(Im({ tl.x, br.y + 6.0f }), kAccent, label);
        }

        if (m_Gesture == Gesture::Marquee) {
            const Vec2 a = ToScreen(m_MarqueeStart);
            const Vec2 b = Vae(ImGui::GetIO().MousePos);
            draw->AddRectFilled(Im(a), Im(b), kAccentBg);
            draw->AddRect(Im(a), Im(b), kAccent, 0.0f, 0, 1.0f);
        }

        draw->PopClipRect();
    }

    // ------------------------------------------------------------------------------ window

    void Canvas::FrameAll(EditorState& state) {
        const doc::Node* screen = state.Doc().Find(state.ActiveScreen());
        if (!screen || m_ViewportSize.x < 1.0f) return;
        const Vec2 size = m_LastScreenSize.x > 1.0f ? m_LastScreenSize : DesignSize(state);
        if (size.x <= 0.0f || size.y <= 0.0f) return;

        const f32 margin = 48.0f;
        m_Zoom = std::clamp(std::min((m_ViewportSize.x - margin * 2.0f) / size.x,
                                     (m_ViewportSize.y - margin * 2.0f) / size.y),
                            kMinZoom, kMaxZoom);
        m_Pan = size * 0.5f - m_ViewportSize / (2.0f * m_Zoom);
    }

    void Canvas::ZoomTo(f32 zoom) {
        const Vec2 center = m_Pan + m_ViewportSize / (2.0f * m_Zoom);
        m_Zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
        m_Pan = center - m_ViewportSize / (2.0f * m_Zoom);
    }

    void Canvas::BeginTextEdit(EditorState& state, Uuid node) {
        const doc::Node* target = state.Doc().Find(node);
        if (!target || target->kind != doc::NodeKind::Text) return;

        m_EditingText = node;
        m_EditBefore = state.Doc().GetProp(node, doc::Prop::Text) == doc::Value{}
                     ? std::string{}
                     : std::get<std::string>(state.Doc().GetProp(node, doc::Prop::Text));
        std::snprintf(m_EditBuffer, sizeof m_EditBuffer, "%s", m_EditBefore.c_str());
        m_EditFocus = true;
        state.Select(node);
    }

    void Canvas::EndTextEdit(bool keep) {
        (void)keep;
        m_EditingText = Uuid::Invalid();
        m_EditFocus = false;
        m_EditBuffer[0] = '\0';
    }

    // The field itself: an ImGui input parked over the label it is editing, at the size the label
    // is drawn. Studio's chrome is ImGui by design, and a caret of our own in the canvas renderer
    // would be a second text editor to keep correct for no gain.
    void Canvas::DrawTextEditor(EditorState& state) {
        if (!m_EditingText.Valid()) return;
        const doc::Node* node = state.Doc().Find(m_EditingText);
        if (!node) { EndTextEdit(); return; }

        const Rect box = NodeBounds(state, m_EditingText);
        const Vec2 tl = ToScreen(box.pos);
        const Vec2 br = ToScreen({ box.Right(), box.Bottom() });
        const f32 width = std::max(br.x - tl.x, 60.0f);
        const f32 height = std::max(br.y - tl.y, ImGui::GetTextLineHeight() + 8.0f);

        ImGui::SetCursorScreenPos(ImVec2(tl.x, tl.y));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(20, 22, 28, 235));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(93, 130, 228, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushID("##canvas-text-edit");

        if (m_EditFocus) { ImGui::SetKeyboardFocusHere(); m_EditFocus = false; }
        const bool multiline = m_EditBefore.find('\n') != std::string::npos
                            || state.Doc().GetProp(m_EditingText, doc::Prop::TextWrap)
                               != doc::Value{ std::string("none") };
        bool changed = false;
        if (multiline) {
            changed = ImGui::InputTextMultiline("##v", m_EditBuffer, sizeof m_EditBuffer,
                                                ImVec2(width, height));
        } else {
            ImGui::SetNextItemWidth(width);
            changed = ImGui::InputText("##v", m_EditBuffer, sizeof m_EditBuffer);
        }
        if (changed) state.SetProp(m_EditingText, doc::Prop::Text, std::string(m_EditBuffer));

        // Escape puts back what was there; clicking away or Enter (on a single line) keeps it.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.SetProp(m_EditingText, doc::Prop::Text, m_EditBefore);
            state.EndGesture();
            EndTextEdit(false);
        } else if (ImGui::IsItemDeactivated()
                   || (!multiline && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
            state.EndGesture();
            EndTextEdit();
        }

        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    void Canvas::OnImGuiRender(EditorState& state) {
        // With a device the renderer lays the scene out every frame on its way to drawing it.
        // Without one — the headless selftest, a player that has not made its device yet — nobody
        // would, and the input below would be running against a tree that was never laid out.
        if (!m_Device) SyncScene(state, ImGui::GetIO().DeltaTime);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Canvas###Canvas");

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        m_ViewportPos = Vae(ImGui::GetCursorScreenPos());
        m_ViewportSize = { std::max(avail.x, 1.0f), std::max(avail.y, 1.0f) };

        if (m_TargetHandle)
            ImGui::Image(static_cast<ImTextureID>(m_TargetHandle), Im(m_ViewportSize));
        else
            ImGui::Dummy(Im(m_ViewportSize));

        // Dropping a component lands it under the pointer. Placing at the centre of the view is
        // what the library panel does on a click; a drag says where.
        if (ImGui::BeginDragDropTarget()) {
            // An image dropped on a node becomes that node's picture; dropped on empty canvas it
            // becomes an Image of its own. Both are what the gesture obviously means.
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VAE_ASSET")) {
                const Uuid asset{ *static_cast<const u64*>(payload->Data) };
                const Vec2 at = ToDocument(Vae(ImGui::GetIO().MousePos));
                const Uuid onto = PickAt(state, at);
                const doc::Node* node = state.Doc().Find(onto);
                // Artwork becomes a Vector node, a photograph an Image: the difference is whether
                // it is redrawn at the size it ends up or scaled from the size it was decoded at.
                const bool artwork = m_Assets.IsVector(asset);
                const doc::NodeKind wanted = artwork ? doc::NodeKind::Vector
                                                     : doc::NodeKind::Image;
                if (node && node->kind == wanted) {
                    state.SetProp(onto, doc::Prop::Image, doc::AssetRef{ asset });
                } else if (artwork) {
                    state.PlaceArtwork(asset, DropTargetAt(state, at), at, m_Assets.SizeOf(asset),
                                       m_Assets.FollowsText(asset));
                } else {
                    const Uuid placed = state.PlaceInstance("Image", DropTargetAt(state, at), at);
                    if (placed.Valid())
                        state.SetProp(placed, doc::Prop::Image, doc::AssetRef{ asset });
                }
                state.EndGesture();
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VAE_COMPONENT")) {
                const auto* name = static_cast<const char*>(payload->Data);
                const Vec2 at = ToDocument(Vae(ImGui::GetIO().MousePos));
                const Uuid parent = DropTargetAt(state, at);
                // Into a container the position is the container's business, not the pointer's.
                state.PlaceInstance(name, parent,
                                    parent == state.ActiveScreen() ? at : Vec2{ 0.0f, 0.0f });
            }
            ImGui::EndDragDropTarget();
        }

        m_Hovered = ImGui::IsItemHovered();
        HandleInput(state);
        DrawOverlay(state);
        // After the overlay, so the field sits on top of the selection frame it replaces.
        DrawTextEditor(state);
        if (m_Rulers && !m_Preview) DrawRulers(state);

        ImGui::End();
        ImGui::PopStyleVar();
    }

}
