#pragma once

#include "vae/base/Assert.h"
#include "vae/doc/Document.h"
#include "vae/draw/DrawList.h"
#include "vae/layout/LayoutTree.h"
#include "vae/text/TextLayout.h"
#include "vae/motion/Driver.h"
#include "vae/ui/Widget.h"

#include <map>
#include <set>

namespace vae::text { class GlyphAtlas; }

namespace vae::ui {

    class Behavior;
    class UiHost;

    // Where an image or an icon comes from. The view tree never opens a file: a document stores an
    // AssetRef and the host resolves it, so the same tree renders in Studio, in the player and in a
    // headless test where nothing resolves at all.
    class AssetTable {
    public:
        virtual ~AssetTable() = default;
        virtual Ref<gpu::Texture> Image(Uuid asset) const = 0;
        // Artwork, drawn at a size rather than decoded at one. `tint` replaces every colour in
        // the file when it is given, which is how an icon obeys a theme token.
        virtual Ref<gpu::Texture> Vector(Uuid, Vec2, const Color*) const { return {}; }
    };

    struct PaintContext {
        draw::DrawList* list = nullptr;
        // Null in a headless run: boxes still record, text is still measured and laid out, only the
        // glyph quads are skipped. That is what lets every interaction test run without a GPU.
        text::GlyphAtlas* atlas = nullptr;
        const AssetTable* assets = nullptr;
        // Set when painting through a host. Behaviors that draw their own decoration — a caret, a
        // scrollbar thumb, a virtualized list's rows — need it; without one they simply do not draw.
        UiHost* host = nullptr;
        // Device pixels per logical pixel. Layout, hit-testing and every coordinate in the document
        // stay logical; only glyph rasterization cares, because a glyph is the one thing that is a
        // bitmap rather than a shape.
        f32 pixelRatio = 1.0f;
    };

    // The runtime shape of a document: one view per node after instances are expanded, wired to a
    // layout node and, where the node has a Role, to a native behavior.
    //
    // Rebuilt wholesale whenever the document's structure changes. That is affordable because
    // nothing durable lives here — a widget's state is a document property, and hover/focus belong
    // to the host — so a rebuild costs a flatten and not a lost interaction.
    class ViewTree {
    public:
        static constexpr u32 kInvalid = 0xFFFFFFFFu;

        struct View {
            Uuid sourceId;
            Uuid instanceId;                 // which copy of an instance produced it, or Invalid
            Uuid overrideId;                 // the instance a write to this view should land in
            Uuid overrideKey;                // the key that write is filed under
            Uuid authoredId;                 // the authored node this view came from
            u32  parent = kInvalid;
            std::vector<u32> children;
            u32  layoutNode = layout::LayoutTree::kInvalid;

            doc::NodeKind kind = doc::NodeKind::Frame;
            Role role = Role::None;
            std::string name;
            doc::PropBag props;              // as authored, state overlays included
            StateMask state = 0;

            Vec2 scroll{ 0.0f, 0.0f };
            // How far short of filling the box its content falls, for a container that fills from
            // the far edge (Prop::StickToEnd). Content moves down by this much, which is what puts
            // a three-message conversation above the composer instead of under the title.
            f32  stickSlack = 0.0f;
            // One of a repeated container's copies. What a widget changes about it is kept here
            // rather than written to the document, because every copy is the same document node.
            bool repeated = false;
            i32  row = -1;                   // which copy, or -1 outside one
            bool rowRoot = false;            // the copy itself, not something drawn inside it

            bool visible = true;
            bool clip = false;
            Behavior* behavior = nullptr;    // owned by m_Behaviors; stable across vector growth
        };

        ViewTree();
        ~ViewTree();
        ViewTree(const ViewTree&) = delete;
        ViewTree& operator=(const ViewTree&) = delete;

        void Build(doc::Document& document, Uuid root);
        Uuid RootId() const { return m_RootId; }
        void Rebuild();
        void Clear();

        void Layout(Vec2 available);
        // Where the root sits in absolute space. Overlays use it: a popover lays itself out at its
        // hug size and is only then placed against its anchor.
        void SetOrigin(Vec2 origin);
        Vec2 Origin() const { return m_Origin; }
        Vec2 RootSize() const;
        void Paint(PaintContext& context) const;

        // Topmost visible view containing the point, honouring clipping. kInvalid for a miss.
        u32 HitTest(Vec2 point) const;
        // Walks up from `view` to the first ancestor (or itself) that owns a behavior.
        u32 BehaviorOwner(u32 view) const;

        u32 Root() const { return m_Root; }
        u32 ViewCount() const { return static_cast<u32>(m_Views.size()); }
        // Asserted rather than clamped: unlike Bounds, there is no sensible empty View to hand
        // back, and every caller here walks ViewCount() or has already checked Valid(). An index
        // that is out of range is a bug in the caller, and kInvalid is UINT32_MAX.
        const View& At(u32 view) const { VAE_CORE_ASSERT(Valid(view), "view out of range"); return m_Views[view]; }
        View& At(u32 view) { VAE_CORE_ASSERT(Valid(view), "view out of range"); return m_Views[view]; }
        bool Valid(u32 view) const { return view < m_Views.size(); }
        // Absolute, scroll-adjusted. Painting and hit-testing read exactly the same numbers, so a
        // control can never be drawn somewhere it cannot be clicked.
        // A view that is not there has no box, rather than whatever is at that index. Callers ask
        // about nodes that have just been created and not laid out yet, and kInvalid is UINT32_MAX.
        const Rect& Bounds(u32 view) const;
        const Rect& ClipBounds(u32 view) const;
        Vec2 ContentSize(u32 view) const;

        // First view under `root` (inclusive) with this role, in painter order.
        u32 FindRole(u32 root, Role role) const;
        std::vector<u32> FindAllRoles(u32 root, Role role) const;
        u32 FindByName(std::string_view name) const;
        u32 ViewOf(WidgetId id) const;

        // The resolved property set a view should render with: authored props, then the overlays
        // for whatever states are active, with tokens resolved against the active theme.
        doc::PropBag Resolved(u32 view) const;
        doc::Value ResolvedProp(u32 view, doc::Prop prop) const;
        f32  Number(u32 view, doc::Prop prop, f32 fallback = 0.0f) const;
        bool Flag(u32 view, doc::Prop prop, bool fallback = false) const;
        std::string Str(u32 view, doc::Prop prop, std::string fallback = {}) const;

        // Writes land on the instance as an override when the view came from one, and on the node
        // itself otherwise — so toggling one checkbox does not toggle every instance of it.
        void SetViewProp(u32 view, doc::Prop prop, doc::Value value);
        // The same write, on the view only. For anything that is looking at the app rather than
        // editing it — the debugger's frozen values — so a hold leaves no mark on the document and
        // costs no rebuild.
        void SetViewPropLocal(u32 view, doc::Prop prop, doc::Value value);

        void SetState(u32 view, StateBit bit, bool on);

        // --- rows ---------------------------------------------------------------------------------
        // What a repeated container repeats over. Kept here rather than in the document because
        // rows are data an app is showing, not a design anyone drew — and, like scroll offsets,
        // they have to survive the rebuild that showing them causes.
        void SetRows(WidgetId widget, doc::RowTable rows);
        void ClearRows(WidgetId widget);
        const doc::RowTable* RowsOf(WidgetId widget) const;
        // Draw repeated containers with the sample rows their Prop::Sample carries, for the ones
        // no app has handed real rows to. The Studio canvas turns this on and nothing else does:
        // a designer needs to see the template they are styling, and a running app showing
        // invented people would be a bug rather than a convenience.
        void ShowSampleRows(bool on);
        bool ShowingSampleRows() const { return m_ShowSampleRows; }
        // The copy a view is part of, or kInvalid. The innermost one: a click on a label inside
        // the third message of the second channel happened in the third message, and that is what
        // it should be able to say.
        u32 RowOwner(u32 view) const;
        // "Keep this scroller at the bottom." Deferred to the next layout on purpose: the rows
        // that make it taller do not exist yet when a script asks — it has only just handed them
        // over — so scrolling now would scroll to the end of the list as it was.
        void KeepAtEnd(WidgetId widget);

        // --- motion ------------------------------------------------------------------------------
        // State changes ease rather than snap. A button whose fill jumps between rest and hover
        // reads as a redraw; the same button over 140ms reads as a button. Animated values live in
        // the driver and never reach the document — they are what the widget looks like right now,
        // not what the designer drew.
        struct Motion {
            bool enabled = true;
            f32 duration = 0.14f;
            motion::Easing curve = motion::Easing::OutCubic;
        };
        void SetMotion(Motion motion) { m_Motion = motion; }
        const Motion& MotionSettings() const { return m_Motion; }
        // Advances every transition. True while anything is still moving, which is what tells an
        // idle main loop it owes another frame.
        bool Animate(f32 dt);
        bool Animating() const { return m_Driver.Busy(); }
        // What a view is styled by: its own state plus the state of the widget it is a part of.
        // A checkbox's tick, a switch's track and a button's label are all just nodes, and a
        // designer restyles them for a state by writing `checked:fill` on the part itself.
        StateMask EffectiveState(u32 view) const;
        bool IsEnabled(u32 view) const;

        // Runtime-only presentation. A slider's filled track, a scrollbar thumb, the panel a tab
        // shows — all of it is transient, so none of it goes through the document or the undo
        // stack. A rebuild drops these and the behaviors' Sync puts them back.
        void SetLayoutStyle(u32 view, const layout::LayoutStyle& style);
        const layout::LayoutStyle& LayoutStyleOf(u32 view) const;
        void SetRuntimeVisible(u32 view, bool visible);
        // Scrolling is runtime state, not a document edit. A wheel tick that dirtied the document
        // would rebuild the tree and land on the undo stack, which is not what scrolling is.
        void SetScroll(u32 view, Vec2 scroll);
        // Where this view's children actually sit: the scroll offset, less the slack a
        // stick-to-end container is holding them down by. Everything that turns layout rects into
        // screen rects has to agree on this or the content measures itself.
        Vec2 ScrollOffset(u32 view) const;
        // True once since the last layout: a behavior moved something. Lets the host re-solve only
        // when a knob or a thumb actually moved rather than laying out twice every frame.
        bool ConsumeLayoutDirty();

        doc::Document& Document() const { return *m_Document; }
        layout::LayoutTree& LayoutNodes() { return m_Layout; }
        text::TextStyle StyleFor(u32 view) const;

    private:
        struct Frame {
            Rect rect;
            Rect clip;
            Corners clipCorners;
            f32  opacity = 1.0f;
            bool visible = true;
        };

        void BuildViews();
        void AttachBehaviors();
        void ComputeFrames();
        void ComputeFrame(u32 view, Vec2 origin, const Frame& parent);
        Vec2 MeasureText(u32 view, Vec2 available) const;
        void PaintView(u32 view, PaintContext& context) const;
        void PaintText(u32 view, const doc::PropBag& props, const Rect& rect,
                       PaintContext& context) const;

        doc::Document* m_Document = nullptr;
        Uuid m_RootId = Uuid::Invalid();
        u32  m_Root = kInvalid;

        std::vector<View> m_Views;
        std::vector<Frame> m_Frames;
        std::vector<Scope<Behavior>> m_Behaviors;
        layout::LayoutTree m_Layout;
        // Scroll offsets outlive the tree they were made in. A rebuild renumbers every view, and a
        // list that jumped back to the top because something unrelated changed is a bug the user
        // sees immediately.
        std::map<WidgetId, Vec2> m_ScrollState;
        // What a widget changed about a repeated copy. Keyed the same way scroll is, and kept for
        // the same reason: the copy has no node of its own, and a rebuild must not forget it.
        std::map<WidgetId, doc::PropBag> m_RuntimeProps;
        // The rows behind each repeated container, by the container's identity.
        std::map<WidgetId, doc::RowTable> m_Rows;
        // Parsed once per rebuild rather than once per copy: a repeated container asks for its
        // rows as many times as it has copies, and the text behind them does not change between.
        const doc::RowTable* SampleRowsFor(Uuid node) const;
        void ApplyStickToEnd();
        const Frame& FrameOf(u32 view) const;
        mutable std::unordered_map<Uuid, doc::RowTable> m_SampleRows;
        bool m_ShowSampleRows = false;
        // Scrollers asked to sit at the end once the layout that decides where that is has run.
        std::set<WidgetId> m_ScrollToEnd;
        // What the end WAS, per stick-to-end container. A scroller sitting at the end stays there
        // when new content arrives; one the reader has scrolled up from stays where they left it,
        // which is the difference between following a conversation and being yanked out of one.
        std::map<WidgetId, f32> m_StickEnd;
        // Which properties are worth easing: the ones a state overlay can change, that have
        // something to interpolate. A token resolves to a colour before it gets here.
        static const std::vector<doc::Prop>& Animatable();
        doc::Value ResolvedStatic(u32 view, doc::Prop prop) const;
        // How much a state says to lighten or darken a colour it did not name outright. Zero when
        // the state named the colour itself, or said nothing about it.
        f32 StateTint(u32 view, doc::Prop prop) const;
        void StartTransitions(u32 view, const doc::PropBag& before);

        motion::Driver m_Driver;
        Motion m_Motion{};
        bool m_LayoutDirty = false;
        Vec2 m_Available{ 0.0f, 0.0f };
        Vec2 m_Origin{ 0.0f, 0.0f };
    };

}
