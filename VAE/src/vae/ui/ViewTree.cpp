#include "vaepch.h"
#include "vae/ui/ViewTree.h"

#include "vae/text/FontDB.h"
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
        // The rows an app handed over travel into the flatten, because they decide how many copies
        // a repeated container has and what each one draws.
        const auto flat = m_Document->Flatten(m_RootId,
            [this](Uuid node, Uuid instance) -> const doc::RowTable* {
                const auto it = m_Rows.find(WidgetId{ node, instance });
                return it == m_Rows.end() ? nullptr : &it->second;
            });
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
        for (u32 i = 0; i < m_Views.size(); ++i) {
            View& view = m_Views[i];
            if (view.role == Role::None) continue;
            Scope<Behavior> behavior = MakeBehavior(view.role);
            if (!behavior) continue;
            view.behavior = behavior.get();
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

    Vec2 ViewTree::MeasureText(u32 view, Vec2 available) const {
        const std::string content = Str(view, doc::Prop::Text);
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
        return text::TextLayout::Measure(content, style, maxWidth, wrap);
    }

    void ViewTree::Layout(Vec2 available) {
        if (m_Root == kInvalid) return;
        m_Available = available;

        for (u32 i = 0; i < m_Views.size(); ++i) {
            const View& view = m_Views[i];
            m_Layout.SetExcluded(view.layoutNode, !view.visible);
            if (!view.children.empty()) continue;
            if (view.kind == doc::NodeKind::Text)
                m_Layout.SetMeasure(view.layoutNode, [this, i](Vec2 box) { return MeasureText(i, box); });
        }

        m_Layout.Compute(m_Views[m_Root].layoutNode, available);
        m_LayoutDirty = false;
        ComputeFrames();

        // Now that the boxes are real, the scrollers that were told to stay at the end can be:
        // a chat that has just been handed a new message ends up showing it.
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
        const Vec2 childOrigin = frame.rect.pos - view.scroll;
        for (u32 child : view.children) ComputeFrame(child, childOrigin, forChildren);
    }

    Vec2 ViewTree::ContentSize(u32 view) const {
        if (!Valid(view) || m_Views[view].children.empty()) return { 0.0f, 0.0f };
        const Vec2 origin = m_Frames[view].rect.pos - m_Views[view].scroll;
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
        if (const doc::Value* value = node.props.Find(prop)) {
            const doc::Value resolved = m_Document->ResolveValue(*value);
            if (const f32 tint = StateTint(view, prop); tint != 0.0f)
                if (const Color* colour = std::get_if<Color>(&resolved))
                    return Tinted(*colour, tint);
            return resolved;
        }
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
        doc::PropBag bag = m_Views[view].props;
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
        View& node = m_Views[view];
        if (const doc::Value* existing = node.props.Find(prop); existing && *existing == value)
            return;
        node.props.Set(prop, std::move(value));
        if (prop == doc::Prop::ScrollX) node.scroll.x = node.props.Number(doc::Prop::ScrollX, 0.0f);
        if (prop == doc::Prop::ScrollY) node.scroll.y = node.props.Number(doc::Prop::ScrollY, 0.0f);
        m_LayoutDirty = true;
    }

    void ViewTree::SetViewProp(u32 view, doc::Prop prop, doc::Value value) {
        if (!Valid(view) || !m_Document) return;
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
        return m_Driver.Advance(dt);
    }

    void ViewTree::SetLayoutStyle(u32 view, const layout::LayoutStyle& style) {
        if (!Valid(view)) return;
        if (m_Layout.Style(m_Views[view].layoutNode) == style) return;
        m_Layout.SetStyle(m_Views[view].layoutNode, style);
        m_LayoutDirty = true;
    }

    const layout::LayoutStyle& ViewTree::LayoutStyleOf(u32 view) const {
        return m_Layout.Style(m_Views[view].layoutNode);
    }

    void ViewTree::SetRuntimeVisible(u32 view, bool visible) {
        if (!Valid(view) || m_Views[view].visible == visible) return;
        m_Views[view].visible = visible;
        m_LayoutDirty = true;
    }

    void ViewTree::SetRows(WidgetId widget, doc::RowTable rows) {
        m_Rows[widget] = std::move(rows);
    }

    void ViewTree::ClearRows(WidgetId widget) { m_Rows.erase(widget); }

    void ViewTree::KeepAtEnd(WidgetId widget) { m_ScrollToEnd.insert(widget); }

    const doc::RowTable* ViewTree::RowsOf(WidgetId widget) const {
        const auto it = m_Rows.find(widget);
        return it == m_Rows.end() ? nullptr : &it->second;
    }

    u32 ViewTree::RowOwner(u32 view) const {
        for (u32 at = view; at != kInvalid; at = m_Views[at].parent)
            if (m_Views[at].rowRoot) return at;
        return kInvalid;
    }

    void ViewTree::SetScroll(u32 view, Vec2 scroll) {
        if (!Valid(view)) return;
        m_Views[view].scroll = scroll;
        m_Views[view].props.Set(doc::Prop::ScrollX, scroll.x);
        m_Views[view].props.Set(doc::Prop::ScrollY, scroll.y);
        m_ScrollState[WidgetId{ m_Views[view].sourceId, m_Views[view].instanceId }] = scroll;
        ComputeFrames();
    }

    bool ViewTree::ConsumeLayoutDirty() {
        const bool dirty = m_LayoutDirty;
        m_LayoutDirty = false;
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
        const std::string content = props.Text(doc::Prop::Text);
        if (content.empty()) return;

        const text::TextStyle style = StyleFor(index);
        if (!style.font) return;

        const auto wrap = WrapFromName(props.Text(doc::Prop::TextWrap, "word"));
        const auto align = AlignFromName(props.Text(doc::Prop::TextAlign, "left"));
        const auto result = text::TextLayout::Layout(content, style, rect.size.x, wrap, align);
        const Color colour = WithAlpha(props.Colour(doc::Prop::TextColor, { 1, 1, 1, 1 }),
                                       m_Frames[index].opacity);
        text::DrawGlyphs(*context.list, *context.atlas, result, rect.pos, colour, style.size,
                         context.pixelRatio);
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
