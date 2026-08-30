#include "vaepch.h"
#include "vae/ui/ViewTree.h"

#include "vae/base/Utf8.h"
#include "vae/text/FontDB.h"
#include "vae/text/TextCache.h"
#include "vae/text/TextDraw.h"
#include "vae/ui/Behavior.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vae::ui {

    namespace {
        // Strongest first, the mirror of the overlay order in Widget.cpp — a single-property lookup
        // has to answer exactly what a full overlay pass would.
        constexpr std::array<StateBit, 7> kLookupOrder{
            StateBit::Disabled, StateBit::Pressed, StateBit::Hovered, StateBit::Open,
            StateBit::Checked, StateBit::Selected, StateBit::Focused,
        };

        constexpr Rect kUnclipped{ { -1.0e6f, -1.0e6f }, { 2.0e6f, 2.0e6f } };

        text::WrapMode WrapFromName(std::string_view name) {
            if (name == "none") return text::WrapMode::None;
            if (name == "char") return text::WrapMode::Char;
            return text::WrapMode::Word;
        }

        text::TextAlign AlignFromName(std::string_view name) {
            if (name == "center") return text::TextAlign::Center;
            if (name == "right")  return text::TextAlign::Right;
            return text::TextAlign::Left;
        }

        Color WithAlpha(Color colour, f32 opacity) { colour.a *= opacity; return colour; }
    }

    ViewTree::ViewTree() = default;
    ViewTree::~ViewTree() = default;

    void ViewTree::Clear() {
        m_NeedsSolve = true;
        m_HasBreakpoints = false;
        m_Views.clear();
        m_Frames.clear();
        m_Behaviors.clear();
        m_Layout.Clear();
        m_Root = kInvalid;
    }

    void ViewTree::Build(doc::Document& document, Uuid root) {
        m_Document = &document;
        m_RootId = root;
        Rebuild();
    }

    void ViewTree::Rebuild() {
        Clear();
        if (!m_Document || !m_Document->Contains(m_RootId)) return;
        BuildViews();
        AttachBehaviors();
    }

    void ViewTree::BuildViews() {
        m_SampleRows.clear();
        // The rows an app handed over travel into the flatten, because they decide how many copies
        // a repeated container has and what each one draws. Real rows first, then the sample ones
        // the designer typed: a list that has been handed data shows the data, on the canvas as
        // much as anywhere else.
        // What each repeated container is asked to build. `m_RowMetrics` is last frame's
        // measurements — the extent of a row, the box that clips it, how far that box is scrolled —
        // and it is rewritten here with what this flatten actually decided, so the next solve can
        // check whether the window still covers what is on screen.
        std::map<WidgetId, RowMetrics> measured;
        const auto flat = m_Document->Flatten(m_RootId,
            [this](Uuid node, Uuid instance) -> const doc::RowTable* {
                const auto it = m_Rows.find(WidgetId{ node, instance });
                if (it != m_Rows.end()) return &it->second;
                return SampleRowsFor(node);
            },
            [this, &measured](Uuid node, Uuid instance, u32 total) {
                const WidgetId id{ node, instance };
                const doc::RowWindow window = WindowFor(node, instance, total);
                RowMetrics metrics;
                if (const auto it = m_RowMetrics.find(id); it != m_RowMetrics.end()) metrics = it->second;
                metrics.total = total;
                metrics.first = window.count == 0 ? 0 : window.first;
                metrics.count = window.count == 0 ? total : window.count;
                measured[id] = metrics;
                return window;
            });
        m_RowMetrics = std::move(measured);
        m_Views.reserve(flat.size());

        for (const auto& node : flat) {
            View view;
            view.sourceId = node.sourceId;
            view.instanceId = node.instanceId;
            view.overrideId = node.overrideId;
            view.overrideKey = node.overrideKey;
            view.authoredId = node.authoredId;
            view.parent = node.parent;
            view.kind = node.kind;
            view.name = node.name;
            view.props = node.props;
            view.repeated = node.repeated;
            view.row = node.row;
            view.rowRoot = node.rowRoot;
            // What a widget already changed about this copy, put back. Keyed on the copy's own
            // identity, so row three keeps its tick and row one keeps not having one.
            if (view.repeated) {
                if (const auto it = m_RuntimeProps.find(WidgetId{ node.sourceId, node.instanceId });
                    it != m_RuntimeProps.end())
                    for (const auto& [prop, value] : it->second.Known())
                        if (doc::IsSet(value)) view.props.Set(prop, value);
            }

            if (const auto* role = view.props.Find(doc::Prop::Role))
                if (const auto* text = std::get_if<std::string>(role))
                    view.role = RoleFromName(*text).value_or(Role::None);

            // Selectable text is the field behaviour over a label, held read-only. A checkbox on
            // the label you already styled, rather than a component you have to swap it for and
            // restyle — the behaviour finds the text node it is standing on, which is this one.
            if (view.role == Role::None && view.kind == doc::NodeKind::Text
                && view.props.Flag(doc::Prop::Selectable, false)) {
                view.role = Role::TextInput;
                view.props.Set(doc::Prop::ReadOnly, true);
                view.props.Set(doc::Prop::Multiline,
                               view.props.Text(doc::Prop::TextWrap, "word") != "none");
            }

            const doc::Node* source = m_Document->Find(node.sourceId);
            view.visible = (source == nullptr || source->visible)
                        && view.props.Flag(doc::Prop::Visible, true);
            view.clip = view.props.Flag(doc::Prop::ClipContent, false);
            view.scroll = { view.props.Number(doc::Prop::ScrollX, 0.0f),
                            view.props.Number(doc::Prop::ScrollY, 0.0f) };
            if (auto it = m_ScrollState.find(WidgetId{ view.sourceId, view.instanceId });
                it != m_ScrollState.end())
                view.scroll = it->second;

            // What the nodes above hand down. The parent is already complete — the flatten is
            // depth-first, so a parent is always built before its children — so this is one lookup
            // rather than a walk, and the chain resolves itself as the tree is built.
            if (node.parent != UINT32_MAX) {
                const View& above = m_Views[node.parent];
                view.inherited = above.inherited;
                for (const doc::Prop prop : Inheritable())
                    if (const doc::Value* value = above.props.Find(prop))
                        view.inherited.Set(prop, *value);
            }

            // Does anything here answer to a width? Checked once while building rather than per
            // layout, because the answer is no for almost every design and the pass it gates is a
            // whole extra solve.
            if (!m_HasBreakpoints && !view.props.Custom().empty())
                for (const doc::Breakpoint& breakpoint : m_Document->Breakpoints()) {
                    const std::string prefix = breakpoint.name + ':';
                    for (const auto& [key, unused] : view.props.Custom())
                        if (key.starts_with(prefix)) { m_HasBreakpoints = true; break; }
                    if (m_HasBreakpoints) break;
                }
            view.baseLayout = node.layout;

            const u32 index = static_cast<u32>(m_Views.size());
            const u32 parentLayout = node.parent == UINT32_MAX
                                   ? layout::LayoutTree::kInvalid
                                   : m_Views[node.parent].layoutNode;
            view.layoutNode = m_Layout.Add(node.layout, parentLayout);

            if (node.parent != UINT32_MAX) m_Views[node.parent].children.push_back(index);
            m_Views.push_back(std::move(view));
        }

        // A container that names one of its children shows that one and no other. Four drawings
        // of one screen — loading, failed, empty, the content — stacked in the same box is what a
        // designer draws otherwise, and only one of them can be looked at.
        //
        // The others are hidden rather than dropped: they stay addressable, so a script can fill
        // in the failure message before switching to it.
        for (View& view : m_Views) {
            const std::string shown = view.props.Text(doc::Prop::Shown);
            if (shown.empty()) continue;
            for (const u32 child : view.children)
                if (m_Views[child].name != shown) m_Views[child].visible = false;
        }

        // A repeated container that names a selected row marks that copy selected, so the row the
        // designer styled with `selected:fill` is the row that lights up. Same property a list
        // uses, and the same meaning, on a container whose rows are real nodes.
        for (View& view : m_Views) {
            if (view.children.empty()) continue;
            const doc::Value* value = view.props.Find(doc::Prop::SelectedIndex);
            if (!value) continue;
            const auto* index = std::get_if<f32>(value);
            if (!index) continue;
            for (const u32 child : view.children)
                if (m_Views[child].row >= 0 && m_Views[child].row == static_cast<i32>(*index))
                    m_Views[child].state = WithState(m_Views[child].state, StateBit::Selected, true);
        }

        // The root of a tree someone asked to build is always shown. Hiding a subtree is expressed
        // by not building it — which is what lets a dropdown's menu be an invisible node in the
        // main tree and a visible one in the overlay built from it.
        if (!m_Views.empty()) m_Views[0].visible = true;

        m_Root = m_Views.empty() ? kInvalid : 0;
        m_Frames.assign(m_Views.size(), Frame{});
    }

    void ViewTree::AttachBehaviors() {
        Touched();
        m_BehaviorViews.clear();
        for (u32 i = 0; i < m_Views.size(); ++i) {
            View& view = m_Views[i];
            if (view.role == Role::None) continue;
            Scope<Behavior> behavior = MakeBehavior(view.role);
            if (!behavior) continue;
            view.behavior = behavior.get();
            m_BehaviorViews.push_back(i);
            m_Behaviors.push_back(std::move(behavior));
        }
        // Disabled is a document property, not an interaction outcome, so it is true before the
        // first event rather than after the first hover.
        for (u32 i = 0; i < m_Views.size(); ++i)
            SetState(i, StateBit::Disabled, !m_Views[i].props.Flag(doc::Prop::Enabled, true));
    }

    // ------------------------------------------------------------------------------ layout

    text::TextStyle ViewTree::StyleFor(u32 view) const {
        text::FontRequest request;
        request.family = Str(view, doc::Prop::FontFamily);
        request.size = Number(view, doc::Prop::FontSize, 14.0f);
        request.weight = static_cast<text::FontWeight>(
            static_cast<u16>(Number(view, doc::Prop::FontWeight, 400.0f)));
        request.slant = Flag(view, doc::Prop::FontItalic) ? text::FontSlant::Italic
                                                          : text::FontSlant::Normal;
        text::TextStyle style = text::FontDB::Get().Style(request);
        style.lineHeight = Number(view, doc::Prop::LineHeight, 0.0f);
        style.letterSpacing = Number(view, doc::Prop::LetterSpacing, 0.0f);
        return style;
    }

    // What a label actually says: its translation when there is one, and the text the designer
    // typed otherwise. Measuring and painting both come through here, because a caption that is
    // measured in one language and drawn in another is a layout that does not fit.
    std::string ViewTree::TextOf(u32 view) const {
        if (m_Strings) {
            const std::string key = Str(view, doc::Prop::TextKey);
            if (!key.empty()) {
                const std::string_view translated = m_Strings->Find(key);
                if (!translated.empty()) return std::string(translated);
            }
        }
        return Str(view, doc::Prop::Text);
    }

    Vec2 ViewTree::MeasureText(u32 view, Vec2 available) const {
        const std::string content = TextOf(view);
        if (content.empty()) {
            // An empty label still occupies a line: a field that collapses to nothing the moment it
            // is cleared makes the whole form jump.
            const text::TextStyle style = StyleFor(view);
            return style.font ? Vec2{ 0.0f, style.font->Metrics(style.size).LineHeight() }
                              : Vec2{ 0.0f, 0.0f };
        }
        const text::TextStyle style = StyleFor(view);
        if (!style.font) return { 0.0f, 0.0f };
        const f32 maxWidth = std::isfinite(available.x) ? std::max(available.x, 0.0f) : 0.0f;
        const auto wrap = WrapFromName(Str(view, doc::Prop::TextWrap, "word"));
        return text::TextCache::Measure(content, style, maxWidth, wrap);
    }

    void ViewTree::Layout(Vec2 available) {
        if (m_Root == kInvalid) return;
        // Nothing has happened that could move anything, and the box is the one it was solved
        // against: the tree already holds the answer. Everything that invalidates a solve says so
        // through InvalidateLayout, so this is a gate rather than a guess.
        if (!m_NeedsSolve && available == m_Available) return;
        m_Available = available;
        m_NeedsSolve = false;
        ++m_Solves;
        Touched();

        for (u32 i = 0; i < m_Views.size(); ++i) {
            const View& view = m_Views[i];
            m_Layout.SetExcluded(view.layoutNode, !view.visible);
            if (!view.children.empty()) continue;
            if (view.kind == doc::NodeKind::Text)
                m_Layout.SetMeasure(view.layoutNode, [this, i](Vec2 box) { return MeasureText(i, box); });
        }

        // Pushed every solve rather than on the change, so turning the preview off restores the
        // root's own width without anyone having to remember to.
        m_Layout.SetStyle(m_Views[m_Root].layoutNode, BaseLayoutOf(m_Root));
        m_Layout.Compute(m_Views[m_Root].layoutNode, available);

        // A breakpoint is a fact about a box, and the box is what the solve just decided — so the
        // answer needs one more solve to take effect, and that solve can change the answer.
        // Bounded rather than iterated to a fixed point: two rounds settle every layout that
        // settles at all, and a design that flips between two widths has asked for something
        // contradictory rather than earned more rounds.
        if (m_HasBreakpoints) {
            constexpr int kRounds = 2;
            int round = 0;
            for (; round < kRounds && ApplyBreakpoints(); ++round)
                m_Layout.Compute(m_Views[m_Root].layoutNode, available);
            if (round == kRounds && ApplyBreakpoints())
                VAE_CORE_WARN("layout: breakpoints did not settle in {} rounds — a node is "
                              "changing the width its own query is answered by", kRounds);
        }

        m_LayoutDirty = false;
        ComputeFrames();

        // The boxes are real now, so a virtualized list can be told what it actually shows.
        RecordRowMetrics();

        // Now that the boxes are real, so is "fills from the bottom".
        ApplyStickToEnd();

        // And the scrollers that were told to stay at the end can be: a chat that has just been
        // handed a new message ends up showing it.
        if (m_ScrollToEnd.empty()) return;
        for (u32 i = 0; i < m_Views.size(); ++i) {
            const WidgetId id{ m_Views[i].sourceId, m_Views[i].instanceId };
            if (!m_ScrollToEnd.contains(id)) continue;
            const f32 limit = std::max(ContentSize(i).y - m_Frames[i].rect.size.y, 0.0f);
            if (std::abs(m_Views[i].scroll.y - limit) > 0.01f) {
                SetScroll(i, { m_Views[i].scroll.x, limit });
                ComputeFrames();
            }
        }
        m_ScrollToEnd.clear();
    }

    bool ViewTree::ApplyBreakpoints() {
        const std::vector<doc::Breakpoint>& breakpoints = m_Document->Breakpoints();
        bool changed = false;
        for (u32 i = 0; i < m_Views.size(); ++i) {
            View& view = m_Views[i];
            // The width the node was GIVEN, which is the honest thing to query: a node sized to
            // its own content has no width to answer with until its content is laid out, and
            // asking anyway is how a container query goes circular.
            const f32 width = m_Layout.NodeRect(view.layoutNode).size.x;
            const u32 mask = m_Document->BreakpointsAt(width);
            const layout::LayoutStyle styled =
                ApplyLayoutBreakpoints(BaseLayoutOf(i), view.props, breakpoints, mask);

            if (mask != view.breakpoints) { view.breakpoints = mask; changed = true; }
            if (!(m_Layout.Style(view.layoutNode) == styled)) {
                m_Layout.SetStyle(view.layoutNode, styled);
                changed = true;
            }
        }
        return changed;
    }

    void ViewTree::SetPreviewWidth(f32 width) {
        if (m_PreviewWidth == width) return;
        m_PreviewWidth = width;
        InvalidateLayout();
    }

    layout::LayoutStyle ViewTree::BaseLayoutOf(u32 view) const {
        layout::LayoutStyle style = m_Views[view].baseLayout;
        if (view == m_Root && m_PreviewWidth > 1.0f)
            style.width = layout::Size::Px(m_PreviewWidth);
        return style;
    }

    void ViewTree::SetOrigin(Vec2 origin) {
        m_Origin = origin;
        ComputeFrames();
    }

    Vec2 ViewTree::RootSize() const {
        return m_Root == kInvalid ? Vec2{ 0.0f, 0.0f } : m_Layout.NodeRect(m_Views[m_Root].layoutNode).size;
    }

    void ViewTree::ComputeFrames() {
        if (m_Root == kInvalid) return;
        m_Frames.assign(m_Views.size(), Frame{});
        Frame root;
        root.clip = kUnclipped;
        root.opacity = 1.0f;
        root.visible = true;
        ComputeFrame(m_Root, m_Origin, root);
    }

    void ViewTree::ComputeFrame(u32 index, Vec2 origin, const Frame& parent) {
        const View& view = m_Views[index];
        Frame& frame = m_Frames[index];

        frame.rect = m_Layout.NodeRect(view.layoutNode).Translated(origin);
        frame.visible = parent.visible && view.visible;
        frame.opacity = parent.opacity * view.props.Number(doc::Prop::Opacity, 1.0f);
        frame.clip = parent.clip;
        frame.clipCorners = parent.clipCorners;

        // A clipping node clips its CHILDREN, not itself — its own border and shadow have to escape
        // the box they are drawn on.
        Frame forChildren = frame;
        if (view.clip) {
            forChildren.clip = frame.clip.Intersect(frame.rect);
            const f32 radius = Number(index, doc::Prop::CornerRadius, 0.0f);
            forChildren.clipCorners = radius > 0.0f ? Corners{ radius } : parent.clipCorners;
        }

        // Scroll moves the children, never the scroller.
        const Vec2 childOrigin = frame.rect.pos - ScrollOffset(index);
        for (u32 child : view.children) ComputeFrame(child, childOrigin, forChildren);
    }

    Vec2 ViewTree::ContentSize(u32 view) const {
        if (!Valid(view) || m_Views[view].children.empty()) return { 0.0f, 0.0f };
        // The same offset the frames were built with, so the answer is how tall the content is
        // and not how far down something pushed it.
        const Vec2 origin = m_Frames[view].rect.pos - ScrollOffset(view);
        Vec2 extent{ 0.0f, 0.0f };
        for (u32 child : m_Views[view].children) {
            // A scrollbar is chrome, not content. Counting the bar — which spans the whole
            // viewport by construction — would report content exactly as tall as the view and no
            // scroller would ever scroll.
            const Role role = m_Views[child].role;
            if (role == Role::Track || role == Role::Thumb) continue;
            const Rect& rect = m_Frames[child].rect;
            extent.x = std::max(extent.x, rect.Right() - origin.x);
            extent.y = std::max(extent.y, rect.Bottom() - origin.y);
        }
        return extent;
    }

    // ------------------------------------------------------------------------------ props

    // What the document says, transitions ignored: where a property is heading, not where it is.
    // Starting an animation against the animated value would target the widget at itself.
    f32 ViewTree::StateTint(u32 view, doc::Prop prop) const {
        if (!Valid(view)) return 0.0f;
        const View& node = m_Views[view];
        const StateMask state = EffectiveState(view);
        if (state == 0 || node.props.Custom().empty()) return 0.0f;

        // The strongest active state that says anything at all about this property wins, the same
        // way naming a colour does — a pressed widget is not also hovered as far as looks go.
        for (StateBit bit : kLookupOrder) {
            if (!HasState(state, bit)) continue;
            if (node.props.Find(StateKey(bit, prop))) return 0.0f;
            if (const doc::Value* value = node.props.Find(StateTintKey(bit)))
                if (const f32* amount = std::get_if<f32>(value)) return *amount;
        }
        return 0.0f;
    }

    doc::Value ViewTree::ResolvedStatic(u32 view, doc::Prop prop) const {
        if (!Valid(view)) return {};
        const View& node = m_Views[view];
        const StateMask state = EffectiveState(view);
        if (state != 0 && !node.props.Custom().empty()) {
            for (StateBit bit : kLookupOrder) {
                if (!HasState(state, bit)) continue;
                if (const doc::Value* value = node.props.Find(StateKey(bit, prop)))
                    return m_Document->ResolveValue(*value);
            }
        }
        // A width the node is answering to beats what it says unconditionally, and loses to a
        // state, which is the more immediate statement about the same node.
        if (node.breakpoints != 0 && !node.props.Custom().empty()) {
            const std::vector<doc::Breakpoint>& breakpoints = m_Document->Breakpoints();
            for (std::size_t i = breakpoints.size(); i-- > 0;) {
                if ((node.breakpoints & (1u << i)) == 0) continue;
                if (const doc::Value* value = node.props.Find(BreakpointKey(breakpoints[i].name, prop)))
                    return m_Document->ResolveValue(*value);
            }
        }
        if (const doc::Value* value = node.props.Find(prop)) {
            const doc::Value resolved = m_Document->ResolveValue(*value);
            if (const f32 tint = StateTint(view, prop); tint != 0.0f)
                if (const Color* colour = std::get_if<Color>(&resolved))
                    return Tinted(*colour, tint);
            return resolved;
        }
        // Nothing here says what this should be, so ask what was handed down. Last, because a
        // value the node names is a decision about this node and beats one made further up.
        if (const doc::Value* value = node.inherited.Find(prop))
            return m_Document->ResolveValue(*value);
        return {};
    }

    doc::Value ViewTree::ResolvedProp(u32 view, doc::Prop prop) const {
        if (!Valid(view)) return {};
        // Mid-transition, what the widget looks like is the animation's value, not the document's.
        if (m_Motion.enabled) {
            const View& node = m_Views[view];
            const motion::Key key{ node.instanceId, node.sourceId, static_cast<u16>(prop), 0 };
            if (const doc::Value animated = m_Driver.Current(key); doc::IsSet(animated))
                return animated;
        }
        return ResolvedStatic(view, prop);
    }

    doc::PropBag ViewTree::Resolved(u32 view) const {
        if (!Valid(view)) return {};
        // Inherited underneath, authored over the top, state overlays over that: weakest first, so
        // each layer is allowed to be overruled by a more specific decision.
        doc::PropBag bag = m_Views[view].inherited;
        m_Views[view].props.MergeInto(bag);
        ApplyBreakpointOverlay(bag, m_Views[view].props, m_Document->Breakpoints(),
                               m_Views[view].breakpoints);
        ApplyStateOverlay(bag, m_Views[view].props, EffectiveState(view));

        doc::PropBag out;
        for (const auto& [prop, value] : bag.Known()) {
            doc::Value resolved = m_Document->ResolveValue(value);
            if (const f32 tint = StateTint(view, prop); tint != 0.0f)
                if (const Color* colour = std::get_if<Color>(&resolved))
                    resolved = Tinted(*colour, tint);
            out.Set(prop, std::move(resolved));
        }
        // The same override the single-property read applies, so painting and hit-testing never
        // disagree about what colour something is.
        if (m_Motion.enabled)
            for (const doc::Prop prop : Animatable()) {
                const motion::Key key{ m_Views[view].instanceId, m_Views[view].sourceId,
                                       static_cast<u16>(prop), 0 };
                if (const doc::Value animated = m_Driver.Current(key); doc::IsSet(animated))
                    out.Set(prop, animated);
            }
        for (const auto& [key, value] : bag.Custom()) {
            if (key.find(':') != std::string::npos) continue;   // state overlays are not properties
            out.Set(key, m_Document->ResolveValue(value));
        }
        return out;
    }

    f32 ViewTree::Number(u32 view, doc::Prop prop, f32 fallback) const {
        const doc::Value value = ResolvedProp(view, prop);
        if (const f32* number = std::get_if<f32>(&value)) return *number;
        if (const bool* flag = std::get_if<bool>(&value)) return *flag ? 1.0f : 0.0f;
        return fallback;
    }

    bool ViewTree::Flag(u32 view, doc::Prop prop, bool fallback) const {
        const doc::Value value = ResolvedProp(view, prop);
        if (const bool* flag = std::get_if<bool>(&value)) return *flag;
        if (const f32* number = std::get_if<f32>(&value)) return *number != 0.0f;
        return fallback;
    }

    std::string ViewTree::Str(u32 view, doc::Prop prop, std::string fallback) const {
        const doc::Value value = ResolvedProp(view, prop);
        if (const auto* text = std::get_if<std::string>(&value)) return *text;
        return fallback;
    }

    void ViewTree::SetViewPropLocal(u32 view, doc::Prop prop, doc::Value value) {
        if (!Valid(view)) return;
        Touched();
        View& node = m_Views[view];
        if (const doc::Value* existing = node.props.Find(prop); existing && *existing == value)
            return;
        node.props.Set(prop, std::move(value));
        if (prop == doc::Prop::ScrollX) node.scroll.x = node.props.Number(doc::Prop::ScrollX, 0.0f);
        if (prop == doc::Prop::ScrollY) node.scroll.y = node.props.Number(doc::Prop::ScrollY, 0.0f);
        m_LayoutDirty = true;
        m_NeedsSolve = true;
    }

    void ViewTree::SetViewProp(u32 view, doc::Prop prop, doc::Value value) {
        if (!Valid(view) || !m_Document) return;
        Touched();
        View& node = m_Views[view];

        // A repeated copy has no node of its own — every copy is the one node the designer drew —
        // so writing to the document would tick every row at once. It goes to runtime state keyed
        // on the copy instead, which is the same place a scroll offset already lives.
        if (node.repeated) {
            m_RuntimeProps[WidgetId{ node.sourceId, node.instanceId }].Set(prop, value);
            SetViewPropLocal(view, prop, std::move(value));
            return;
        }

        // The write has to land where the read came from. A view produced by an instance reads
        // through the override table, so writing straight to the node would edit the master and
        // move every other instance with it.
        if (node.overrideId.Valid())
            m_Document->SetOverride(node.overrideId, node.overrideKey, prop, value);
        else
            m_Document->SetProp(node.sourceId, prop, value);

        // Also apply locally so a behavior that reads back what it just wrote sees it this frame,
        // before the host has had a chance to rebuild.
        node.props.Set(prop, std::move(value));
        m_NeedsSolve = true;
        if (prop == doc::Prop::ScrollX) node.scroll.x = node.props.Number(doc::Prop::ScrollX, 0.0f);
        if (prop == doc::Prop::ScrollY) node.scroll.y = node.props.Number(doc::Prop::ScrollY, 0.0f);
    }

    StateMask ViewTree::EffectiveState(u32 view) const {
        if (!Valid(view)) return 0;
        StateMask mask = 0;
        // Stop at the nearest behavior: that is the widget this part belongs to. Going further
        // would let a hovered panel make every control inside it look hovered.
        for (u32 i = view; i != kInvalid; i = m_Views[i].parent) {
            mask |= m_Views[i].state;
            if (m_Views[i].behavior) break;
        }
        return mask;
    }

    const std::vector<doc::Prop>& ViewTree::Inheritable() {
        // The set CSS inherits, and for the same reason: these describe how text looks, and text
        // is drawn by nodes far below the one that decided what it should look like. Nothing about
        // a box — fill, radius, padding, a shadow — is here, because a card inside a card is not
        // the same drawing twice.
        static const std::vector<doc::Prop> kProps{
            doc::Prop::FontFamily, doc::Prop::FontSize, doc::Prop::FontWeight,
            doc::Prop::FontItalic, doc::Prop::TextColor, doc::Prop::LineHeight,
            doc::Prop::LetterSpacing, doc::Prop::TextAlign, doc::Prop::TextWrap,
        };
        return kProps;
    }

    u32 ViewTree::BreakpointsOf(u32 view) const {
        return Valid(view) ? m_Views[view].breakpoints : 0u;
    }

    std::string_view ViewTree::NarrowestBreakpoint(u32 view) const {
        if (!Valid(view) || !m_Document) return {};
        return m_Document->NarrowestAt(m_Layout.NodeRect(m_Views[view].layoutNode).size.x);
    }

    const doc::PropBag& ViewTree::InheritedProps(u32 view) const {
        static const doc::PropBag kNone;
        return Valid(view) ? m_Views[view].inherited : kNone;
    }

    const std::vector<doc::Prop>& ViewTree::Animatable() {
        static const std::vector<doc::Prop> kProps{
            doc::Prop::Fill, doc::Prop::FillOpacity, doc::Prop::Stroke, doc::Prop::StrokeWidth,
            doc::Prop::CornerRadius, doc::Prop::Opacity, doc::Prop::TextColor,
            doc::Prop::ShadowColor, doc::Prop::ShadowBlur, doc::Prop::ShadowSpread,
            doc::Prop::ShadowOffset,
        };
        return kProps;
    }

    void ViewTree::SetState(u32 view, StateBit bit, bool on) {
        if (!Valid(view)) return;
        const StateMask before = m_Views[view].state;
        const StateMask after = WithState(before, bit, on);
        if (before == after) return;

        // A state change on a parent restyles its parts too — a hovered button dims nothing, but a
        // disabled one greys its own label. So the whole subtree is sampled, down to the next
        // widget, which is exactly the boundary EffectiveState already respects.
        std::vector<std::pair<u32, doc::PropBag>> was;
        if (m_Motion.enabled && m_Document) {
            std::vector<u32> queue{ view };
            for (std::size_t at = 0; at < queue.size(); ++at) {
                const u32 node = queue[at];
                doc::PropBag bag;
                for (const doc::Prop prop : Animatable()) {
                    const doc::Value value = ResolvedProp(node, prop);
                    if (doc::IsSet(value) && motion::Lanes(doc::TypeOf(value)) > 0)
                        bag.Set(prop, value);
                }
                if (!bag.Known().empty()) was.emplace_back(node, std::move(bag));
                for (const u32 child : m_Views[node].children)
                    if (!m_Views[child].behavior) queue.push_back(child);
            }
        }

        m_Views[view].state = after;
        Touched();
        // A state overlay can name a property that decides a box — `hovered:fontSize` is a label
        // that grows — so a state change is a reason to lay out again.
        m_NeedsSolve = true;
        for (const auto& [node, bag] : was) StartTransitions(node, bag);
    }

    void ViewTree::StartTransitions(u32 view, const doc::PropBag& before) {
        if (!Valid(view)) return;
        const View& node = m_Views[view];

        motion::Options options;
        options.duration = m_Motion.duration;
        options.curve = m_Motion.curve;

        for (const auto& [prop, from] : before.Known()) {
            const motion::Key key{ node.instanceId, node.sourceId, static_cast<u16>(prop), 0 };
            const doc::Value to = ResolvedStatic(view, prop);

            if (doc::TypeOf(from) != doc::TypeOf(to) || from == to) {
                // Nowhere to go. Cancelling matters: a transition still running toward the value
                // this state used to have would carry on to a target nothing is asking for.
                m_Driver.Cancel(key);
                continue;
            }
            // `from` came through the driver, so an interrupted transition sets off from where it
            // visibly is; To() retargets in place rather than restarting.
            m_Driver.To(key, from, to, options);
        }
    }

    bool ViewTree::Animate(f32 dt) {
        if (!m_Motion.enabled) { m_Driver.Clear(); return false; }
        const bool busy = m_Driver.Advance(dt);
        // A transition on a clipping node's corner radius decides where its children are clipped,
        // so a frame that is still moving is a frame whose boxes are worth recomputing. Nothing
        // else in Animatable() reaches the layout, so this ends the moment the motion does.
        if (busy) m_NeedsSolve = true;
        return busy;
    }

    void ViewTree::SetLayoutStyle(u32 view, const layout::LayoutStyle& style) {
        if (!Valid(view)) return;
        if (m_Layout.Style(m_Views[view].layoutNode) == style) return;
        Touched();
        m_Layout.SetStyle(m_Views[view].layoutNode, style);
        m_LayoutDirty = true;
        m_NeedsSolve = true;
    }

    const layout::LayoutStyle& ViewTree::LayoutStyleOf(u32 view) const {
        return m_Layout.Style(m_Views[view].layoutNode);
    }

    void ViewTree::SetRuntimeVisible(u32 view, bool visible) {
        if (!Valid(view) || m_Views[view].visible == visible) return;
        Touched();
        m_Views[view].visible = visible;
        m_LayoutDirty = true;
        m_NeedsSolve = true;
    }

    void ViewTree::SetRows(WidgetId widget, doc::RowTable rows) {
        Touched();
        m_Rows[widget] = std::move(rows);
        m_NeedsSolve = true;
    }

    void ViewTree::ClearRows(WidgetId widget) {
        m_Rows.erase(widget);
        m_NeedsSolve = true;
    }

    void ViewTree::SetStrings(const doc::StringTable* strings) {
        if (m_Strings == strings) return;
        Touched();
        m_Strings = strings;
        // Every label with a key says something different now, and text decides layout.
        m_LayoutDirty = true;
        m_NeedsSolve = true;
    }

    void ViewTree::ShowSampleRows(bool on) {
        if (m_ShowSampleRows == on) return;
        m_ShowSampleRows = on;
        Rebuild();
    }

    // Only a container that already repeats: sample rows are what the repeat draws, and a frame
    // that someone left a table on should not silently start copying itself.
    const doc::RowTable* ViewTree::SampleRowsFor(Uuid node) const {
        if (!m_ShowSampleRows || !m_Document) return nullptr;
        const auto cached = m_SampleRows.find(node);
        if (cached != m_SampleRows.end())
            return cached->second.columns.empty() ? nullptr : &cached->second;

        const doc::Node* source = m_Document->Find(node);
        if (!source || !source->props.Find(doc::Prop::Repeat)) return nullptr;
        doc::RowTable table = doc::ParseRowText(source->props.Text(doc::Prop::Sample));
        const bool empty = table.columns.empty();
        const doc::RowTable& stored = m_SampleRows.emplace(node, std::move(table)).first->second;
        return empty ? nullptr : &stored;
    }

    // --------------------------------------------------------------------------- virtualized lists

    // The window a container gets, from what the last frame measured about it.
    //
    // Nothing is guessed. Until a frame has built a copy and laid it out there is no row extent, so
    // the answer is "all of them" — which is what a list short enough to fit wanted anyway, and
    // which gives the next frame the measurement it needs for a list that is not.
    doc::RowWindow ViewTree::WindowFor(Uuid node, Uuid instance, u32 total) const {
        // Short lists are not worth the machinery, and keeping them off this path means every
        // design that already worked keeps behaving exactly as it did.
        if (total <= kVirtualizeAbove) return {};

        // Nothing has been laid out yet, so there is nothing to window against — but building a
        // million rows to find out how tall one of them is would be the whole problem again. A
        // screenful is built, measured, and replaced on the next frame by the real answer.
        const auto it = m_RowMetrics.find(WidgetId{ node, instance });
        if (it == m_RowMetrics.end() || it->second.rowExtent <= 0.0f)
            return { 0, std::min(kBootstrapRows, total), 0.0f, 0.0f, false };
        const RowMetrics& metrics = it->second;
        // ...and the evidence says nothing clips it, so every copy really is on screen.
        if (metrics.viewport <= 0.0f) return { 0, 0, 0.0f, 0.0f, true };

        // A margin either side, so a scroll of a few rows does not need a rebuild and a fast one
        // has already-built rows to land on.
        const f32 margin = static_cast<f32>(kOverscan) * metrics.rowExtent;
        const f32 top = std::max(metrics.offset - margin, 0.0f);
        const u32 first = std::min(static_cast<u32>(top / metrics.rowExtent), total);
        const f32 span = metrics.viewport + 2.0f * margin;
        const u32 count = std::min(static_cast<u32>(span / metrics.rowExtent) + 2, total - first);

        // A spacer stands where `first` copies would have been, and a stack puts a gap after it —
        // so it is that much shorter than the space it stands for, or every row below it is one
        // gap too low.
        const u32 rest = total - first - count;
        doc::RowWindow window;
        window.measured = true;
        window.first = first;
        window.count = count;
        window.before = first == 0 ? 0.0f : static_cast<f32>(first) * metrics.rowExtent - metrics.gap;
        window.after  = rest == 0  ? 0.0f : static_cast<f32>(rest) * metrics.rowExtent - metrics.gap;
        return window;
    }

    void ViewTree::RecordRowMetrics() {
        if (m_RowMetrics.empty()) return;

        // Every container the flatten windowed, found by the copies it produced. One pass over the
        // views rather than a lookup per container, because a screen has few of either and this
        // runs after a solve rather than per frame.
        for (u32 i = 0; i < m_Views.size(); ++i) {
            const View& view = m_Views[i];
            if (!view.rowRoot || view.parent == kInvalid) continue;
            const View& container = m_Views[view.parent];
            const auto it = m_RowMetrics.find(WidgetId{ container.sourceId, container.instanceId });
            if (it == m_RowMetrics.end()) continue;
            RowMetrics& metrics = it->second;

            const layout::LayoutStyle& style = m_Layout.Style(container.layoutNode);
            const bool horizontal = style.axis == layout::Axis::Row;
            const Rect box = m_Layout.NodeRect(view.layoutNode);
            const f32 extent = (horizontal ? box.size.x : box.size.y) + style.gap;
            if (extent > 0.0f) { metrics.rowExtent = extent; metrics.gap = style.gap; }

            // The box that decides what can be seen: the nearest ancestor that clips, or the
            // container itself when it is the scroller. Nothing clipping means every copy is
            // genuinely on screen and there is no window to build.
            metrics.viewport = 0.0f;
            metrics.offset = 0.0f;
            for (u32 up = view.parent; up != kInvalid; up = m_Views[up].parent) {
                if (!m_Views[up].clip) continue;
                const Rect clipBox = m_Layout.NodeRect(m_Views[up].layoutNode);
                metrics.viewport = horizontal ? clipBox.size.x : clipBox.size.y;
                metrics.offset = horizontal ? m_Views[up].scroll.x : m_Views[up].scroll.y;
                break;
            }
        }

    }

    bool ViewTree::WindowMoved() const {
        for (const auto& [widget, metrics] : m_RowMetrics) {
            const doc::RowWindow window = WindowFor(widget.node, widget.instance, metrics.total);
            const u32 first = window.count == 0 ? 0 : window.first;
            const u32 count = window.count == 0 ? metrics.total : window.count;
            if (first != metrics.first || count != metrics.count) return true;
        }
        return false;
    }

    void ViewTree::KeepAtEnd(WidgetId widget) {
        m_ScrollToEnd.insert(widget);
        // The scroll that answers this happens inside Layout, so Layout has to run.
        m_NeedsSolve = true;
    }

    // A container that fills from its far edge, in the two states it is ever in: short content
    // held against the bottom of the box, and long content scrolled to the end of itself. Doing
    // both here rather than in a justification is the whole point — a justification cannot know
    // that the content has outgrown the box, and pushes it out of the top when it does.
    void ViewTree::ApplyStickToEnd() {
        bool moved = false;
        std::map<WidgetId, f32> ends;
        for (u32 i = 0; i < m_Views.size(); ++i) {
            View& view = m_Views[i];
            if (!view.props.Flag(doc::Prop::StickToEnd, false)) continue;

            const f32 box = m_Frames[i].rect.size.y;
            const f32 content = ContentSize(i).y;
            const WidgetId id{ view.sourceId, view.instanceId };

            const f32 slack = std::max(box - content, 0.0f);
            if (std::abs(view.stickSlack - slack) > 0.01f) { view.stickSlack = slack; moved = true; }

            // Pinned unless the reader has scrolled away from where the end used to be. A
            // container being laid out for the first time is pinned: a conversation opens on the
            // newest message.
            const f32 end = std::max(content - box, 0.0f);
            const auto previous = m_StickEnd.find(id);
            const bool pinned = previous == m_StickEnd.end()
                             || view.scroll.y >= previous->second - 0.5f;
            ends.emplace(id, end);
            if (pinned && std::abs(view.scroll.y - end) > 0.01f) {
                view.scroll.y = end;
                view.props.Set(doc::Prop::ScrollY, end);
                m_ScrollState[id] = view.scroll;
                moved = true;
            }
        }
        m_StickEnd.swap(ends);
        if (moved) ComputeFrames();
    }

    const doc::RowTable* ViewTree::RowsOf(WidgetId widget) const {
        const auto it = m_Rows.find(widget);
        return it == m_Rows.end() ? nullptr : &it->second;
    }

    u32 ViewTree::RowOwner(u32 view) const {
        for (u32 at = view; at != kInvalid; at = m_Views[at].parent)
            if (m_Views[at].rowRoot) return at;
        return kInvalid;
    }

    // Frames lag the views by a layout pass, and a caller can ask about a node it created a
    // moment ago — kInvalid is UINT32_MAX, and indexing with it is how a missing box became a
    // crash rather than an empty rect.
    const ViewTree::Frame& ViewTree::FrameOf(u32 view) const {
        static const Frame kNowhere{};
        return view < m_Frames.size() ? m_Frames[view] : kNowhere;
    }

    const Rect& ViewTree::Bounds(u32 view) const { return FrameOf(view).rect; }
    const Rect& ViewTree::ClipBounds(u32 view) const { return FrameOf(view).clip; }

    Vec2 ViewTree::ScrollOffset(u32 view) const {
        if (!Valid(view)) return { 0.0f, 0.0f };
        const View& node = m_Views[view];
        return { node.scroll.x, node.scroll.y - node.stickSlack };
    }

    void ViewTree::SetScroll(u32 view, Vec2 scroll) {
        if (!Valid(view)) return;
        Touched();
        // Scrolling a virtualized list moves its window, and the window is decided from settled
        // boxes — so the solve has to run again to notice. Only when there is a virtualized list
        // at all; an ordinary scroller still scrolls without one.
        if (!m_RowMetrics.empty()) m_NeedsSolve = true;
        m_Views[view].scroll = scroll;
        m_Views[view].props.Set(doc::Prop::ScrollX, scroll.x);
        m_Views[view].props.Set(doc::Prop::ScrollY, scroll.y);
        m_ScrollState[WidgetId{ m_Views[view].sourceId, m_Views[view].instanceId }] = scroll;
        ComputeFrames();
    }

    bool ViewTree::ConsumeLayoutDirty() {
        const bool dirty = m_LayoutDirty;
        m_LayoutDirty = false;
        // The caller asks this to decide whether to lay out again, so saying yes has to mean the
        // second pass is allowed to do something.
        if (dirty) m_NeedsSolve = true;
        return dirty;
    }

    bool ViewTree::IsEnabled(u32 view) const {
        return Valid(view) && !HasState(m_Views[view].state, StateBit::Disabled);
    }

    // ------------------------------------------------------------------------------ paint

    void ViewTree::Paint(PaintContext& context) const {
        if (m_Root == kInvalid || !context.list) return;
        PaintView(m_Root, context);
    }

    void ViewTree::PaintView(u32 index, PaintContext& context) const {
        const View& view = m_Views[index];
        const Frame& frame = m_Frames[index];
        if (!frame.visible || frame.opacity <= 0.001f) return;

        // Nothing that lands entirely outside the box it is clipped to is worth recording. Only
        // for a subtree that is actually clipped — a child of a non-clipping parent is allowed to
        // draw outside it, so its own clip is what decides, one node at a time.
        //
        // This is what makes a long list cheap: a thousand rows in a scroller that shows thirty
        // used to be a thousand rows of quads and glyphs submitted every frame, clipped in the
        // fragment shader after the fact.
        if (frame.clip.Intersect(frame.rect).Empty()) {
            // A node that clips its children to a box which is itself off-screen takes the whole
            // subtree with it. One that does not clip only skips its own drawing: a child of it is
            // allowed to sit outside its box, and its own test below is what decides.
            if (view.clip) return;
            for (u32 child : view.children) PaintView(child, context);
            return;
        }

        const doc::PropBag props = Resolved(index);
        const f32 radius = props.Number(doc::Prop::CornerRadius, 0.0f);
        const Corners corners{ radius };
        const f32 opacity = frame.opacity;

        context.list->PushClip(frame.clip, frame.clipCorners);

        const Color shadowColour = props.Colour(doc::Prop::ShadowColor, { 0, 0, 0, 0 });
        if (shadowColour.a > 0.0f) {
            draw::ShadowSpec shadow;
            shadow.color = WithAlpha(shadowColour, opacity);
            shadow.blur = props.Number(doc::Prop::ShadowBlur, 12.0f);
            shadow.spread = props.Number(doc::Prop::ShadowSpread, 0.0f);
            const doc::Value offset = props.Find(doc::Prop::ShadowOffset)
                                    ? *props.Find(doc::Prop::ShadowOffset) : doc::Value{};
            if (const Vec2* value = std::get_if<Vec2>(&offset)) shadow.offset = *value;
            context.list->AddShadow(frame.rect, shadow, corners);
        }

        const Color fill = props.Colour(doc::Prop::Fill, { 0, 0, 0, 0 });
        const f32 strokeWidth = props.Number(doc::Prop::StrokeWidth, 0.0f);
        const Color strokeColour = props.Colour(doc::Prop::Stroke, { 0, 0, 0, 0 });
        const bool hasImage = view.kind == doc::NodeKind::Image && context.assets != nullptr;
        // Artwork is redrawn at the size it ended up, not scaled from whatever size it was
        // decoded at — which is the entire reason to import an icon as a vector.
        const bool hasVector = view.kind == doc::NodeKind::Vector && context.assets != nullptr;

        if (fill.a > 0.0f || strokeWidth > 0.0f || hasImage || hasVector) {
            draw::Paint paint = draw::Paint::Solid(
                WithAlpha(fill, opacity * props.Number(doc::Prop::FillOpacity, 1.0f)));
            const doc::Value asset = props.Find(doc::Prop::Image) ? *props.Find(doc::Prop::Image)
                                                                  : doc::Value{};
            if (hasImage) {
                if (const auto* ref = std::get_if<doc::AssetRef>(&asset)) {
                    if (Ref<gpu::Texture> texture = context.assets->Image(ref->id))
                        paint = draw::Paint::Image(std::move(texture), { 1, 1, 1, opacity });
                }
            } else if (hasVector) {
                // On a vector, Fill is the ink rather than a background: recolouring an icon
                // through a token is what artwork is for, and a box behind it is not.
                const Color* ink = fill.a > 0.0f ? &fill : nullptr;
                paint = draw::Paint::Solid({ 0.0f, 0.0f, 0.0f, 0.0f });
                if (const auto* ref = std::get_if<doc::AssetRef>(&asset)) {
                    const Vec2 pixels = frame.rect.size * std::max(context.pixelRatio, 0.1f);
                    if (Ref<gpu::Texture> texture = context.assets->Vector(ref->id, pixels, ink))
                        paint = draw::Paint::Image(std::move(texture), { 1, 1, 1, opacity });
                }
            }
            const draw::Stroke stroke{ strokeWidth, WithAlpha(strokeColour, opacity) };
            context.list->AddRect(frame.rect, paint, corners, stroke);
        }

        if (view.kind == doc::NodeKind::Text) PaintText(index, props, frame.rect, context);

        if (view.behavior && context.host) {
            WidgetContext widget{ const_cast<ViewTree&>(*this), *context.host, index };
            view.behavior->OnPaint(widget, context);
        }

        context.list->PopClip();

        // Children draw inside this node's clip, which ComputeFrame already folded into theirs.
        for (u32 child : view.children) PaintView(child, context);
    }

    void ViewTree::PaintText(u32 index, const doc::PropBag& props, const Rect& rect,
                             PaintContext& context) const {
        if (!context.atlas) return;
        const std::string content = TextOf(index);
        if (content.empty()) return;

        const text::TextStyle style = StyleFor(index);
        if (!style.font) return;

        const auto wrap = WrapFromName(props.Text(doc::Prop::TextWrap, "word"));
        const auto align = AlignFromName(props.Text(doc::Prop::TextAlign, "left"));
        // Cached: the same label, in the same style, at the same width, shapes once and is drawn
        // from then on. A repeated container makes this the difference between shaping one row and
        // shaping every copy of it.
        const text::TextLayoutResult& result =
            text::TextCache::Layout(content, style, rect.size.x, wrap, align);
        const Color colour = WithAlpha(props.Colour(doc::Prop::TextColor, { 1, 1, 1, 1 }),
                                       m_Frames[index].opacity);
        text::DrawGlyphs(*context.list, *context.atlas, result, rect.pos, colour, style.size,
                         context.pixelRatio);
    }

    // Laid out exactly as PaintText lays it out — same content, style, width, wrap and alignment,
    // out of the same cache — because a box drawn around a character that the shaper put somewhere
    // else is worse than no box at all.
    std::vector<Rect> ViewTree::CharacterBoxes(u32 view) const {
        std::vector<Rect> boxes;
        if (!Valid(view) || m_Views[view].kind != doc::NodeKind::Text) return boxes;

        const std::string content = TextOf(view);
        if (content.empty()) return boxes;
        const text::TextStyle style = StyleFor(view);
        if (!style.font) return boxes;

        const doc::PropBag& props = m_Views[view].props;
        const Rect rect = Bounds(view);
        const text::TextLayoutResult& layout = text::TextCache::Layout(
            content, style, rect.size.x, WrapFromName(props.Text(doc::Prop::TextWrap, "word")),
            AlignFromName(props.Text(doc::Prop::TextAlign, "left")));
        const f32 lineHeight = style.font->Metrics(style.size).LineHeight();
        const f32 ascent = style.font->Metrics(style.size).ascent;

        // One entry per cluster the shaper produced, in byte order. Glyphs come back in *visual*
        // order — an RTL run is already reversed — so they are gathered by byte offset rather than
        // by position, and a mark sitting on the glyph before it joins that glyph's cluster
        // instead of claiming a box of its own.
        struct Cluster { std::size_t at = 0; f32 left = 0.0f, right = 0.0f, baseline = 0.0f; };
        std::vector<Cluster> clusters;
        for (const text::PositionedGlyph& glyph : layout.glyphs) {
            const auto found = std::find_if(clusters.begin(), clusters.end(),
                                            [&](const Cluster& c) { return c.at == glyph.byteOffset; });
            if (found == clusters.end()) {
                clusters.push_back({ glyph.byteOffset, glyph.pen.x, glyph.pen.x + glyph.advance,
                                     glyph.pen.y });
                continue;
            }
            found->left  = std::min(found->left, glyph.pen.x);
            found->right = std::max(found->right, glyph.pen.x + glyph.advance);
        }
        if (clusters.empty()) return boxes;
        std::sort(clusters.begin(), clusters.end(),
                  [](const Cluster& a, const Cluster& b) { return a.at < b.at; });

        // A cluster covers everything up to the next one, which is how a ligature is known to be
        // two characters wide. Splitting its box evenly between them is an approximation — there
        // is no answer to "where does the f end in fi" — but it is one that stays inside the
        // ligature rather than pointing at the character after it.
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            const Cluster& cluster = clusters[i];
            // The first cluster owns anything before it and the last owns anything after, so the
            // boxes come back one per character of the whole string even when the shaper drew no
            // glyph for some of it — a newline is a character to everything above this line.
            const std::size_t from = i == 0 ? 0 : cluster.at;
            const std::size_t end = i + 1 < clusters.size() ? clusters[i + 1].at : content.size();
            const auto characters = static_cast<u32>(Utf8Length(
                std::string_view(content).substr(from, end - from)));
            const f32 width = (cluster.right - cluster.left)
                            / static_cast<f32>(std::max<u32>(characters, 1));
            for (u32 c = 0; c < std::max<u32>(characters, 1); ++c)
                boxes.push_back({ { rect.pos.x + cluster.left + static_cast<f32>(c) * width,
                                    rect.pos.y + cluster.baseline + ascent },
                                  { width, lineHeight } });
        }
        return boxes;
    }

    // ------------------------------------------------------------------------------ queries

    u32 ViewTree::HitTest(Vec2 point) const {
        for (u32 i = static_cast<u32>(m_Views.size()); i-- > 0; ) {
            const Frame& frame = m_Frames[i];
            if (!frame.visible || frame.opacity <= 0.001f) continue;
            if (!frame.clip.Contains(point) || !frame.rect.Contains(point)) continue;
            return i;
        }
        return kInvalid;
    }

    u32 ViewTree::BehaviorOwner(u32 view) const {
        for (u32 i = view; i != kInvalid; i = m_Views[i].parent)
            if (m_Views[i].behavior) return i;
        return kInvalid;
    }

    u32 ViewTree::FindRole(u32 root, Role role) const {
        if (!Valid(root)) return kInvalid;
        if (m_Views[root].role == role) return root;
        for (u32 child : m_Views[root].children) {
            const u32 found = FindRole(child, role);
            if (found != kInvalid) return found;
        }
        return kInvalid;
    }

    std::vector<u32> ViewTree::FindAllRoles(u32 root, Role role) const {
        std::vector<u32> out;
        if (!Valid(root)) return out;
        // Iterative so a deep tree cannot blow the stack, and in painter order so a dropdown's
        // items come back in the order a designer arranged them.
        std::vector<u32> stack{ root };
        while (!stack.empty()) {
            const u32 view = stack.back();
            stack.pop_back();
            if (m_Views[view].role == role) out.push_back(view);
            const auto& children = m_Views[view].children;
            for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(*it);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    u32 ViewTree::FindByName(std::string_view name) const {
        for (u32 i = 0; i < m_Views.size(); ++i)
            if (m_Views[i].name == name) return i;
        return kInvalid;
    }

    u32 ViewTree::ViewOf(WidgetId id) const {
        for (u32 i = 0; i < m_Views.size(); ++i) {
            if (m_Views[i].sourceId != id.node) continue;
            if (id.instance.Valid() && m_Views[i].instanceId != id.instance) continue;
            return i;
        }
        return kInvalid;
    }

}
