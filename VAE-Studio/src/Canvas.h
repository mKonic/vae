#pragma once

#include "EditorState.h"

#include "vae/draw/Renderer.h"
#include "vae/script/Runtime.h"
#include "vae/text/GlyphAtlas.h"
#include "vae/ui/AssetStore.h"
#include "vae/ui/UiHost.h"

#include <functional>

namespace vae {

    // The design surface: the document rendered through the real runtime, plus everything you do
    // to it with a mouse.
    //
    // Zoom is **pixels per document unit**, not a world half-height. That is the mistake that made
    // Ladle's viewport squish when the window changed shape, and a design canvas has to hold its
    // scale across a resize or a dock rearrange.
    class Canvas {
    public:
        bool Init(gpu::Device& device);
        // A node's box on the canvas, in document space. Public because the selftest asks the same
        // question the overlay does, and asking it twice in two ways is how they drift.
        Rect BoundsOf(EditorState& state, Uuid id) const { return NodeBounds(state, id); }
        // The project's images. The Assets panel reads it to show what a project holds and why an
        // image is not showing; the canvas paints through it.
        ui::AssetStore& Assets() { return m_Assets; }
        // Where a dropped component should land: the container under the pointer if there is one,
        // and the screen otherwise. A card is a container, so dropping onto one has to mean *into*
        // it — the alternative is a button sitting on top of a card it is not part of.
        Uuid DropTargetAt(EditorState& state, Vec2 document) const;
        // What a click at that point selects. Public for the same reason as `BoundsOf`: the
        // selftest has to ask exactly the question the canvas asks, not a second version of it.
        Uuid SelectionAt(EditorState& state, Vec2 document) const { return PickAt(state, document); }
        void Shutdown();

        void OnRender(gpu::CommandList& cmd, EditorState& state);
        void OnImGuiRender(EditorState& state);
        // Document → laid-out view tree. Public and separate from painting because hit-testing,
        // snapping and the headless --selftest all read the geometry without wanting a GPU.
        void SyncScene(EditorState& state, f32 dt);
        // Lay the host out again mid-frame, for the one thing that changes what is on screen
        // between the layout and the paint: a navigation.
        void ResyncAfterNavigation();

        void FrameAll(EditorState& state);

        // Alignment is against the selection's own bounding box, and against the screen when only
        // one thing is selected — Figma's rule, and the one that makes "align left" mean something
        // for a single widget instead of nothing.
        enum class Edge { Left, CentreX, Right, Top, CentreY, Bottom };
        void AlignSelection(EditorState& state, Edge edge);
        void DistributeSelection(EditorState& state, bool horizontal);

        bool Rulers() const { return m_Rulers; }
        void SetRulers(bool on) { m_Rulers = on; }

        // False until the canvas has been laid out once. Framing before that divides by a viewport
        // that does not exist yet, and silently leaves the view wherever it started.
        bool HasViewport() const { return m_ViewportSize.x > 1.0f && m_ViewportSize.y > 1.0f; }
        Vec2 ViewportSize() const { return m_ViewportSize; }
        void ZoomTo(f32 zoom);
        f32  Zoom() const { return m_Zoom; }
        bool Preview() const { return m_Preview; }
        void SetPreview(bool on);

        // Play mode: the document stops being a drawing and starts being the app. Input reaches
        // the widgets, and the script runtime drives them.
        ui::UiHost& Host() { return m_Host; }
        // What to run once the scene is laid out and before it is painted: mount, dispatch, tick,
        // and whatever else the session wants to happen inside a frame rather than between two.
        using Pump = std::function<void(f32 dt)>;
        void SetPump(Pump pump) { m_Pump = std::move(pump); }

        // Screen pixels ↔ document units.
        Vec2 ToDocument(Vec2 screen) const;
        Vec2 ToScreen(Vec2 document) const;
        // Document coordinate at the middle of the visible area — where a newly placed widget goes.
        Vec2 ViewCenter() const { return m_Pan + m_ViewportSize / (2.0f * m_Zoom); }

        // For the --selftest driver: the state that a headless run has to be able to assert on.
        Vec2 Pan() const { return m_Pan; }
        const std::vector<Rect>& Guides() const { return m_GuideRects; }

    private:
        enum class Gesture { None, Pan, Move, Resize, Marquee };
        // Corner and edge handles, in the order they are drawn and hit-tested.
        enum Handle { HandleNone = -1, TopLeft, Top, TopRight, Right,
                      BottomRight, Bottom, BottomLeft, Left, HandleCount };

        // The box the document is laid out in. A screen states its own size; a component master
        // opened for editing usually hugs, so it is given a page to lay out on and then framed by
        // whatever it actually came out as.
        Vec2 DesignSize(EditorState& state) const;

        void EnsureTarget(gpu::Device& device);
        void BuildScene(EditorState& state);
        void DrawGrid(Vec2 viewport);
        void DrawScreenFrame(EditorState& state);
        void DrawOverlay(EditorState& state);
        void DrawRulers(EditorState& state);
        void HandleInput(EditorState& state);
        void ForwardInput();

        Rect SelectionBounds(EditorState& state) const;
        Rect NodeBounds(EditorState& state, Uuid id) const;
        Uuid PickAt(EditorState& state, Vec2 document) const;
        // Whether a node is one the thing on the canvas authored, as opposed to the innards of a
        // component being shown inside it. Reached by walking up to the bound root, so it stays
        // true of anything the page dropped into an instance's slot.
        bool Authored(EditorState& state, Uuid id) const;
        int  PickHandle(EditorState& state, Vec2 screen) const;
        void HandlePoints(const Rect& bounds, Vec2* points, bool* live) const;
        Vec2 SnapMove(EditorState& state, Vec2 proposed, Vec2 size);
        // The pointer's shape follows what a click would do here.
        void UpdateCursor(EditorState& state, Vec2 screen);
        // Gives a filling or double-pinned node a size and a single pin, so a drag can move it.
        void FreezeForMove(EditorState& state);
        void ApplyMove(EditorState& state, Vec2 delta);
        void ApplyResize(EditorState& state, Vec2 mouseDoc);

        draw::Renderer   m_Renderer;
        draw::DrawList   m_List;
        text::GlyphAtlas m_Atlas;
        ui::UiHost       m_Host;
        ui::AssetStore   m_Assets;
        Pump m_Pump;
        gpu::Device*     m_Device = nullptr;
        Ref<gpu::RenderTarget> m_Target;
        u64  m_TargetHandle = 0;
        Uuid m_BoundScreen = Uuid::Invalid();
        bool m_PendingFrame = false;      // the root changed; frame it once it has been laid out

        Vec2 m_Pan{ -80.0f, -80.0f };     // document coordinate at the viewport's top-left
        f32  m_Zoom = 1.0f;
        Vec2 m_ViewportPos{ 0.0f, 0.0f };
        Vec2 m_ViewportSize{ 0.0f, 0.0f };
        bool m_Hovered = false;
        Vec2 m_LastScreenSize{ 0.0f, 0.0f };
        bool m_Preview = false;
        bool m_Rulers  = true;

        Gesture m_Gesture = Gesture::None;
        int     m_Handle = HandleNone;
        Vec2    m_GestureStart{ 0.0f, 0.0f };     // document units
        Vec2    m_MarqueeStart{ 0.0f, 0.0f };
        Rect    m_GestureBounds{};
        std::vector<layout::LayoutStyle> m_GestureStyles;
        std::vector<Rect> m_GuideRects;
    };

}
