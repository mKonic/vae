#include "vaepch.h"
#include "vae/ui/UiHost.h"

#include <algorithm>

namespace vae::ui {

    const char* ActionName(ActionKind kind) {
        switch (kind) {
            case ActionKind::Clicked:          return "clicked";
            case ActionKind::ValueChanged:     return "valueChanged";
            case ActionKind::TextChanged:      return "textChanged";
            case ActionKind::Submitted:        return "submitted";
            case ActionKind::SelectionChanged: return "selectionChanged";
            case ActionKind::Opened:           return "opened";
            case ActionKind::Closed:           return "closed";
            case ActionKind::Dismissed:        return "dismissed";
            case ActionKind::Navigated:        return "navigated";
            case ActionKind::Scrolled:         return "scrolled";
        }
        return "unknown";
    }

    UiHost::UiHost()
        : m_Tree(CreateScope<ViewTree>()), m_Clipboard(CreateScope<MemoryClipboard>()) {}

    UiHost::~UiHost() {
        if (m_Document && m_Observer) m_Document->RemoveObserver(m_Observer);
    }

    void UiHost::SetDocument(doc::Document& document, Uuid root) {
        if (m_Document && m_Observer) m_Document->RemoveObserver(m_Observer);
        m_Document = &document;
        m_RootId = root;
        m_Observer = document.AddObserver([this](Uuid) { m_Dirty = true; });
        m_Overlays.clear();
        m_Focused = m_Hovered = m_Captured = ViewTree::kInvalid;
        m_FocusedId = m_CapturedId = WidgetId{};
        m_Dirty = true;
    }

    void UiHost::SetClipboard(Scope<Clipboard> clipboard) {
        if (clipboard) m_Clipboard = std::move(clipboard);
    }

    const ViewTree& UiHost::ActiveTree() const {
        return const_cast<UiHost*>(this)->ActiveTree();
    }

    std::vector<ViewTree*> UiHost::Trees() {
        std::vector<ViewTree*> trees{ m_Tree.get() };
        trees.reserve(m_Overlays.size() + 1);
        for (auto& overlay : m_Overlays) trees.push_back(overlay->tree.get());
        return trees;
    }

    ViewTree& UiHost::ActiveTree() {
        return m_Overlays.empty() ? *m_Tree : *m_Overlays.back()->tree;
    }

    // ------------------------------------------------------------------------------ frame

    void UiHost::Rebuild() {
        if (!m_Document) return;
        m_Tree->Build(*m_Document, m_RootId);
        SyncBehaviors(*m_Tree);
        for (auto& overlay : m_Overlays) {
            overlay->tree->Build(*m_Document, overlay->contentRoot);
            SyncBehaviors(*overlay->tree);
        }

        // Indices are gone; identities are not. The search covers every tree, because focus can
        // sit in the main tree while a popover is open above it.
        const Target focus = Locate(m_FocusedId);
        m_FocusTree = focus.tree;
        m_Focused = focus.view;
        if (m_FocusTree && m_Focused != ViewTree::kInvalid)
            m_FocusTree->SetState(m_Focused, StateBit::Focused, true);

        const Target capture = Locate(m_CapturedId);
        m_CaptureTree = capture.tree;
        m_Captured = capture.view;
        // A widget captures the pointer because it was pressed, and the press is a fact about the
        // pointer, not about the tree that happened to be standing at the time. Losing it here is
        // how anything that writes the document every frame — an animation, a debugger holding a
        // value — makes the whole app unclickable, with a symptom nothing like its cause.
        if (m_CaptureTree && m_Captured != ViewTree::kInvalid)
            m_CaptureTree->SetState(m_Captured, StateBit::Pressed, true);

        m_Hovered = ViewTree::kInvalid;
        m_HoverTree = nullptr;
        m_Dirty = false;

        // A rebuild without a layout leaves every bound at zero, and the very next click lands on
        // nothing. Input arrives between frames, so this cannot wait for the next Update.
        Settle();

        // Hover is derived from where the pointer is, and the pointer did not move. Recomputing it
        // here keeps a widget lit under a stationary cursor across a rebuild.
        UpdateHover(m_Mouse);
    }

    void UiHost::LayoutAll() {
        m_Tree->Layout(m_Available);
        for (auto& overlay : m_Overlays) {
            overlay->tree->Layout(m_Available);
            if (overlay->modal) {
                // A screen presented as a modal keeps the size it was designed at and sits in the
                // middle of whatever is showing it — which is also what gives "outside" a meaning.
                // A modal widget is laid out across the whole area and draws its own scrim, so it
                // stays at the origin.
                const Vec2 size = overlay->tree->RootSize();
                const Vec2 slack = m_Available - size;
                overlay->tree->SetOrigin({ std::max(slack.x * 0.5f, 0.0f),
                                           std::max(slack.y * 0.5f, 0.0f) });
                continue;
            }
            // A popover opens below its anchor and is nudged back on screen rather than clipped —
            // a menu that runs off the bottom of the window is a menu you cannot use.
            const Vec2 size = overlay->tree->RootSize();
            Vec2 origin{ overlay->anchor.Left(), overlay->anchor.Bottom() + 4.0f };
            if (origin.y + size.y > m_Available.y && overlay->anchor.Top() - size.y - 4.0f >= 0.0f)
                origin.y = overlay->anchor.Top() - size.y - 4.0f;
            origin.x = std::clamp(origin.x, 0.0f, std::max(m_Available.x - size.x, 0.0f));
            origin.y = std::clamp(origin.y, 0.0f, std::max(m_Available.y - size.y, 0.0f));
            overlay->tree->SetOrigin(origin);
        }
    }

    void UiHost::ArrangeAll() {
        auto arrange = [this](ViewTree& tree) {
            for (u32 i = 0; i < tree.ViewCount(); ++i) {
                Behavior* behavior = tree.At(i).behavior;
                if (!behavior) continue;
                WidgetContext context{ tree, *this, i };
                behavior->Arrange(context);
            }
        };
        arrange(*m_Tree);
        for (auto& overlay : m_Overlays) arrange(*overlay->tree);
    }

    void UiHost::Settle() {
        LayoutAll();
        ArrangeAll();

        bool moved = m_Tree->ConsumeLayoutDirty();
        for (auto& overlay : m_Overlays) moved = overlay->tree->ConsumeLayoutDirty() || moved;
        if (moved) LayoutAll();
    }

    void UiHost::SyncBehaviors(ViewTree& tree) {
        for (u32 i = 0; i < tree.ViewCount(); ++i) {
            Behavior* behavior = tree.At(i).behavior;
            if (!behavior) continue;
            WidgetContext context{ tree, *this, i };
            behavior->Sync(context);
        }
    }

    void UiHost::Update(Vec2 available, f32 dt) {
        m_Available = available;
        m_Time += dt;
        if (m_Dirty) Rebuild();
        else Settle();

        // Toasts expire on their own. Walk a copy of the ids: closing mutates the stack.
        std::vector<WidgetId> expired;
        for (auto& overlay : m_Overlays) {
            if (overlay->timeToLive <= 0.0f) continue;
            overlay->timeToLive -= dt;
            if (overlay->timeToLive <= 0.0f) expired.push_back(overlay->owner);
        }
        for (const WidgetId& id : expired) CloseOverlay(id);

        m_AnimationRequested = false;
        TickTree(*m_Tree, dt);
        // By index, re-checking the size: a tick can open or close an overlay — a tooltip does
        // exactly that — and an iterator into the stack it just resized is a crash with no symptom
        // until the day someone puts a tooltip inside a dialog.
        for (std::size_t i = 0; i < m_Overlays.size(); ++i) TickTree(*m_Overlays[i]->tree, dt);

        UpdateHover(m_Mouse);

        // Transitions last, so a state change made this frame — by a hover just resolved above, or
        // by a script — starts from the value it actually had rather than losing its first frame.
        m_Animating = m_Tree->Animate(dt);
        for (auto& overlay : m_Overlays) m_Animating = overlay->tree->Animate(dt) || m_Animating;
        m_Animating = m_Animating || m_AnimationRequested;
    }

    bool UiHost::NeedsFrame() const {
        if (m_Animating || m_Dirty || !m_Actions.empty()) return true;
        if (m_Tree && m_Tree->NeedsLayout()) return true;
        for (const auto& overlay : m_Overlays)
            if (overlay->tree->NeedsLayout() || overlay->timeToLive > 0.0f) return true;
        return false;
    }

    void UiHost::TickTree(ViewTree& tree, f32 dt) {
        for (u32 i = 0; i < tree.ViewCount(); ++i) {
            Behavior* behavior = tree.At(i).behavior;
            if (!behavior) continue;
            WidgetContext context{ tree, *this, i };
            behavior->OnTick(context, dt);
        }
    }

    void UiHost::Paint(PaintContext& context) {
        context.host = this;
        m_Tree->Paint(context);
        for (auto& overlay : m_Overlays) {
            if (overlay->scrim && context.list) PaintScrim(*context.list);
            overlay->tree->Paint(context);
        }
    }

    void UiHost::PaintScrim(draw::DrawList& list) const {
        // The dimming behind a screen presented as a modal. It covers the whole surface rather than
        // the screen underneath it, because the screen underneath may be smaller than the window —
        // and a scrim that stops where a screen stops reads as a rectangle, not as "not this part".
        Color colour{ 0.0f, 0.0f, 0.0f, 0.45f };
        if (m_Document) {
            const doc::Value resolved = m_Document->ResolveValue(doc::Value{ doc::TokenRef{ "scrim" } });
            if (const Color* value = std::get_if<Color>(&resolved)) colour = *value;
        }
        if (colour.a <= 0.0f) return;
        list.AddRect(Rect{ { 0.0f, 0.0f }, m_Available }, draw::Paint::Solid(colour));
    }

    // ------------------------------------------------------------------------------ picking

    UiHost::Target UiHost::Locate(WidgetId id) {
        if (!id.Valid()) return {};
        if (const u32 view = m_Tree->ViewOf(id); view != ViewTree::kInvalid)
            return { m_Tree.get(), view };
        for (auto& overlay : m_Overlays)
            if (const u32 view = overlay->tree->ViewOf(id); view != ViewTree::kInvalid)
                return { overlay->tree.get(), view };
        return {};
    }

    UiHost::Target UiHost::Pick(Vec2 point) {
        for (auto it = m_Overlays.rbegin(); it != m_Overlays.rend(); ++it) {
            const u32 view = (*it)->tree->HitTest(point);
            if (view != ViewTree::kInvalid) return { (*it)->tree.get(), view };
            // A modal owns the whole surface even where it draws nothing, or its scrim would leak
            // clicks to the disabled UI behind it.
            if ((*it)->modal) return { (*it)->tree.get(), ViewTree::kInvalid };
        }
        const u32 view = m_Tree->HitTest(point);
        return { m_Tree.get(), view };
    }

    // ------------------------------------------------------------------------------- screens

    bool UiHost::HasScreen(std::string_view name) const {
        if (!m_Document) return false;
        for (const Uuid screen : m_Document->Screens())
            if (const doc::Node* node = m_Document->Find(screen); node && node->name == name)
                return true;
        return false;
    }

    std::string UiHost::CurrentScreenName() const {
        const doc::Node* node = m_Document ? m_Document->Find(m_RootId) : nullptr;
        return node ? node->name : std::string{};
    }

    bool UiHost::GoToScreen(std::string_view name) {
        if (!m_Document) return false;
        for (const Uuid screen : m_Document->Screens())
            if (const doc::Node* node = m_Document->Find(screen); node && node->name == name)
                return GoToScreen(screen);
        return false;
    }

    bool UiHost::GoToScreen(Uuid screen) {
        if (!m_Document) return false;
        const doc::Node* node = m_Document->Find(screen);
        if (!node || node->kind != doc::NodeKind::Screen) return false;
        if (screen == m_RootId) return false;

        const doc::ScreenKind kind = m_Document->KindOf(screen);
        if (doc::IsOverlayKind(kind)) {
            // Presented, not navigated to. The owner is the screen itself, so closing it by id is
            // the same operation whether a script, a scrim or Escape did it.
            const WidgetId owner{ screen, Uuid::Invalid() };
            if (HasOverlay(owner)) return false;
            OpenOverlay(owner, screen, doc::BlocksBelow(kind));
            if (!m_Overlays.empty()) {
                m_Overlays.back()->dismissOnOutsideClick = doc::DismissesItself(kind);
                m_Overlays.back()->scrim = doc::BlocksBelow(kind);
            }
            return true;
        }

        m_ScreenStack.push_back(m_RootId);
        SetDocument(*m_Document, screen);
        return true;
    }

    void UiHost::RequestNavigation(std::string where) {
        // Last one wins. Two navigations from one click is a bug in the design, and going to both
        // in sequence would leave a screen on the back stack nobody ever saw.
        m_PendingNavigation = std::move(where);
    }

    bool UiHost::ApplyNavigation() {
        if (m_PendingNavigation.empty()) return false;
        const std::string where = std::move(m_PendingNavigation);
        m_PendingNavigation.clear();
        return where == "back" ? GoBack() : GoToScreen(where);
    }

    bool UiHost::GoBack() {
        if (!m_Overlays.empty()) { CloseTopOverlay(); return true; }
        if (m_ScreenStack.empty() || !m_Document) return false;

        // Skip anything that has been deleted since it was pushed, rather than navigating to a
        // screen that no longer exists.
        while (!m_ScreenStack.empty()) {
            const Uuid previous = m_ScreenStack.back();
            m_ScreenStack.pop_back();
            if (const doc::Node* node = m_Document->Find(previous);
                node && node->kind == doc::NodeKind::Screen) {
                SetDocument(*m_Document, previous);
                return true;
            }
        }
        return false;
    }

    void UiHost::SetMotion(ViewTree::Motion motion) {
        m_Tree->SetMotion(motion);
        for (auto& overlay : m_Overlays) overlay->tree->SetMotion(motion);
    }

    void UiHost::UpdateHover(Vec2 point) {
        const Target target = Pick(point);
        const u32 owner = target.view == ViewTree::kInvalid
                        ? ViewTree::kInvalid
                        : target.tree->BehaviorOwner(target.view);

        if (m_HoverTree == target.tree && m_Hovered == owner) return;
        if (m_HoverTree && m_Hovered != ViewTree::kInvalid && m_HoverTree->Valid(m_Hovered))
            m_HoverTree->SetState(m_Hovered, StateBit::Hovered, false);

        m_HoverTree = target.tree;
        m_Hovered = owner;
        // The cursor is the widget's to name, not a switch here's: a slider knows whether it is
        // horizontal, and a role table would have to guess.
        m_Cursor = CursorShape::Arrow;
        if (m_HoverTree && m_Hovered != ViewTree::kInvalid) {
            m_HoverTree->SetState(m_Hovered, StateBit::Hovered, true);
            if (Behavior* behavior = m_HoverTree->At(m_Hovered).behavior) {
                WidgetContext context{ *m_HoverTree, *this, m_Hovered };
                m_Cursor = behavior->CursorOver(context);
            }
        }
    }

    // ------------------------------------------------------------------------------ focus

    void UiHost::Focus(u32 view) {
        ViewTree& tree = ActiveTree();
        if (m_Focused == view && m_FocusTree == &tree) return;

        if (m_FocusTree && m_Focused != ViewTree::kInvalid && m_FocusTree->Valid(m_Focused)) {
            m_FocusTree->SetState(m_Focused, StateBit::Focused, false);
            if (Behavior* behavior = m_FocusTree->At(m_Focused).behavior) {
                WidgetContext context{ *m_FocusTree, *this, m_Focused };
                behavior->OnFocusLost(context);
            }
        }

        m_Focused = view;
        m_FocusTree = view == ViewTree::kInvalid ? nullptr : &tree;
        m_FocusedId = WidgetId{};
        if (view == ViewTree::kInvalid) return;

        tree.SetState(view, StateBit::Focused, true);
        m_FocusedId = { tree.At(view).sourceId, tree.At(view).instanceId };
    }

    void UiHost::CollectFocusables(ViewTree& tree, u32 view, std::vector<u32>& out) const {
        if (!tree.Valid(view)) return;
        const ViewTree::View& node = tree.At(view);
        if (node.behavior && node.behavior->Focusable() && tree.IsEnabled(view)) out.push_back(view);
        for (u32 child : node.children) CollectFocusables(tree, child, out);
    }

    void UiHost::FocusNext(bool backwards) {
        ViewTree& tree = ActiveTree();
        std::vector<u32> order;
        CollectFocusables(tree, tree.Root(), order);
        if (order.empty()) { Focus(ViewTree::kInvalid); return; }

        auto it = std::find(order.begin(), order.end(), m_Focused);
        std::size_t index = 0;
        if (it == order.end()) {
            index = backwards ? order.size() - 1 : 0;
        } else {
            const std::size_t current = static_cast<std::size_t>(it - order.begin());
            index = backwards ? (current + order.size() - 1) % order.size()
                              : (current + 1) % order.size();
        }
        Focus(order[index]);
    }

    void UiHost::Capture(u32 view) {
        ViewTree& tree = ActiveTree();
        m_Captured = view;
        m_CaptureTree = view == ViewTree::kInvalid ? nullptr : &tree;
        m_CapturedId = view == ViewTree::kInvalid
                     ? WidgetId{} : WidgetId{ tree.At(view).sourceId, tree.At(view).instanceId };
    }

    void UiHost::ReleaseCapture() {
        if (m_CaptureTree && m_Captured != ViewTree::kInvalid && m_CaptureTree->Valid(m_Captured)) {
            if (Behavior* behavior = m_CaptureTree->At(m_Captured).behavior) {
                WidgetContext context{ *m_CaptureTree, *this, m_Captured };
                behavior->OnCaptureLost(context);
            }
        }
        m_Captured = ViewTree::kInvalid;
        m_CaptureTree = nullptr;
        m_CapturedId = WidgetId{};
    }

    // ------------------------------------------------------------------------------ dispatch

    bool UiHost::Bubble(ViewTree& tree, u32 from, const Event& event) {
        for (u32 view = from; view != ViewTree::kInvalid; view = tree.At(view).parent) {
            Behavior* behavior = tree.At(view).behavior;
            if (!behavior) continue;
            WidgetContext context{ tree, *this, view };
            if (behavior->OnEvent(context, event)) return true;
        }
        return false;
    }

    bool UiHost::Dispatch(const Event& event) {
        if (m_Dirty) Rebuild();

        switch (event.type) {
            case EventType::MouseMoved: {
                m_Mouse = { event.mouse.x, event.mouse.y };
                if (m_CaptureTree && m_Captured != ViewTree::kInvalid)
                    return Bubble(*m_CaptureTree, m_Captured, event);
                UpdateHover(m_Mouse);
                if (m_HoverTree && m_Hovered != ViewTree::kInvalid)
                    return Bubble(*m_HoverTree, m_Hovered, event);
                return false;
            }

            case EventType::MouseButtonPressed: {
                m_Mouse = { event.button.x, event.button.y };
                const Target target = Pick(m_Mouse);

                // A click outside a dismissible overlay closes it and goes no further: the click
                // that shuts a menu must not also press whatever was under the menu.
                if (!m_Overlays.empty()) {
                    Overlay& top = *m_Overlays.back();
                    const bool inside = target.tree == top.tree.get()
                                     && target.view != ViewTree::kInvalid;
                    if (!inside && top.dismissOnOutsideClick) {
                        CloseTopOverlay();
                        return true;
                    }
                    if (!inside && top.modal) return true;
                }

                if (target.view == ViewTree::kInvalid) { Focus(ViewTree::kInvalid); return false; }
                const u32 owner = target.tree->BehaviorOwner(target.view);
                if (owner != ViewTree::kInvalid && target.tree->At(owner).behavior->Focusable()
                    && target.tree->IsEnabled(owner))
                    Focus(owner);
                else
                    Focus(ViewTree::kInvalid);
                return Bubble(*target.tree, owner == ViewTree::kInvalid ? target.view : owner, event);
            }

            case EventType::MouseButtonReleased: {
                m_Mouse = { event.button.x, event.button.y };
                if (m_CaptureTree && m_Captured != ViewTree::kInvalid) {
                    const bool handled = Bubble(*m_CaptureTree, m_Captured, event);
                    ReleaseCapture();
                    UpdateHover(m_Mouse);
                    return handled;
                }
                const Target target = Pick(m_Mouse);
                if (target.view == ViewTree::kInvalid) return false;
                return Bubble(*target.tree, target.tree->BehaviorOwner(target.view), event);
            }

            case EventType::MouseScrolled: {
                const Target target = Pick(m_Mouse);
                if (target.view == ViewTree::kInvalid) return false;
                // Scrolling bubbles from the view under the cursor, not from a behavior owner: the
                // nearest scrollable ancestor should take it even if a button is in between.
                return Bubble(*target.tree, target.view, event);
            }

            case EventType::KeyPressed: {
                if (event.key.code == Key::Escape && !m_Overlays.empty()) {
                    CloseTopOverlay();
                    return true;
                }
                if (m_FocusTree && m_Focused != ViewTree::kInvalid
                    && Bubble(*m_FocusTree, m_Focused, event))
                    return true;
                if (event.key.code == Key::Tab) {
                    FocusNext((event.mods & Mod::Shift) != 0);
                    return true;
                }
                return false;
            }

            case EventType::KeyReleased:
            case EventType::TextInput: {
                if (!m_FocusTree || m_Focused == ViewTree::kInvalid) return false;
                return Bubble(*m_FocusTree, m_Focused, event);
            }

            default:
                return false;
        }
    }

    bool UiHost::DispatchAll(std::initializer_list<Event> events) {
        bool handled = false;
        for (const Event& event : events) handled = Dispatch(event) || handled;
        return handled;
    }

    // ------------------------------------------------------------------------------ overlays

    void UiHost::OpenOverlay(WidgetId owner, Uuid contentRoot, bool modal, Rect anchor, f32 ttl) {
        if (!m_Document || !m_Document->Contains(contentRoot)) return;
        if (HasOverlay(owner)) return;

        auto overlay = CreateScope<Overlay>();
        overlay->owner = owner;
        overlay->contentRoot = contentRoot;
        overlay->modal = modal;
        overlay->dismissOnOutsideClick = true;
        overlay->anchor = anchor;
        overlay->timeToLive = ttl;
        overlay->tree = CreateScope<ViewTree>();
        overlay->tree->Build(*m_Document, contentRoot);
        SyncBehaviors(*overlay->tree);
        m_Overlays.push_back(std::move(overlay));
        Settle();

        Emit({ ActionKind::Opened, owner.node, owner.instance, {}, {} });
    }

    void UiHost::CloseOverlay(WidgetId owner) {
        auto it = std::find_if(m_Overlays.begin(), m_Overlays.end(),
                               [&](const auto& o) { return o->owner == owner; });
        if (it == m_Overlays.end()) return;

        const bool wasActive = (*it).get() == m_Overlays.back().get();
        m_Overlays.erase(it);

        if (wasActive) {
            // Focus and capture lived in the tree that just went away.
            m_Focused = m_Captured = ViewTree::kInvalid;
            m_FocusTree = m_CaptureTree = nullptr;
            m_FocusedId = m_CapturedId = WidgetId{};
            m_Hovered = ViewTree::kInvalid;
            m_HoverTree = nullptr;
        }
        Emit({ ActionKind::Closed, owner.node, owner.instance, {}, {} });
    }

    void UiHost::CloseTopOverlay() {
        if (m_Overlays.empty()) return;
        CloseOverlay(m_Overlays.back()->owner);
    }

    bool UiHost::HasOverlay(WidgetId owner) const {
        return std::any_of(m_Overlays.begin(), m_Overlays.end(),
                           [&](const auto& o) { return o->owner == owner; });
    }

    // ------------------------------------------------------------------------------ actions

    void UiHost::Emit(Action action) {
        if (action.name.empty() && m_Document) {
            if (const doc::Node* node = m_Document->Find(action.source)) action.name = node->name;
        }
        m_Actions.push_back(std::move(action));
    }

    std::vector<Action> UiHost::TakeActions() {
        std::vector<Action> out;
        out.swap(m_Actions);
        return out;
    }

    bool UiHost::Fired(ActionKind kind, std::string_view name) const {
        return std::any_of(m_Actions.begin(), m_Actions.end(), [&](const Action& action) {
            return action.kind == kind && (name.empty() || action.name == name);
        });
    }

    void UiHost::SetDataSource(WidgetId widget, Ref<ListDataSource> source) {
        if (source) m_DataSources[widget] = std::move(source);
        else m_DataSources.erase(widget);
        m_Dirty = true;
    }

    UiHost::ListDataSource* UiHost::DataSource(WidgetId widget) const {
        auto it = m_DataSources.find(widget);
        return it == m_DataSources.end() ? nullptr : it->second.get();
    }

    void UiHost::Navigate(WidgetId router, std::string route) {
        if (!m_Document) return;
        const std::string current = Route(router);
        if (current == route) return;
        if (!current.empty()) m_History[router].push_back(current);
        Write(router, doc::Prop::Route, doc::Value{ route });
        Emit({ ActionKind::Navigated, router.node, router.instance, {}, doc::Value{ route } });
    }

    bool UiHost::Back(WidgetId router) {
        auto it = m_History.find(router);
        if (it == m_History.end() || it->second.empty()) return false;
        const std::string previous = it->second.back();
        it->second.pop_back();
        Write(router, doc::Prop::Route, doc::Value{ previous });
        Emit({ ActionKind::Navigated, router.node, router.instance, {}, doc::Value{ previous } });
        return true;
    }

    std::string UiHost::Route(WidgetId router) const {
        if (!m_Document) return {};
        const doc::Value value = router.instance.Valid()
            ? [&] {
                  const doc::PropBag bag = m_Document->ResolvedProps(router.instance, router.node);
                  const doc::Value* found = bag.Find(doc::Prop::Route);
                  return found ? *found : doc::Value{};
              }()
            : m_Document->GetProp(router.node, doc::Prop::Route);
        const auto* text = std::get_if<std::string>(&value);
        return text ? *text : std::string{};
    }

    std::size_t UiHost::HistoryDepth(WidgetId router) const {
        auto it = m_History.find(router);
        return it == m_History.end() ? 0 : it->second.size();
    }

    void UiHost::Write(WidgetId widget, doc::Prop prop, doc::Value value) {
        if (!m_Document) return;
        if (widget.instance.Valid()) m_Document->SetOverride(widget.instance, widget.node, prop, value);
        else m_Document->SetProp(widget.node, prop, std::move(value));
    }

    WidgetId UiHost::OverlayOwnerOf(const ViewTree& tree) const {
        for (const auto& overlay : m_Overlays)
            if (overlay->tree.get() == &tree) return overlay->owner;
        return {};
    }

    ViewTree* UiHost::OverlayTreeOf(WidgetId owner) {
        for (auto& overlay : m_Overlays)
            if (overlay->owner == owner) return overlay->tree.get();
        return nullptr;
    }

    const TextEditState* UiHost::FindEditState(WidgetId widget) const {
        auto it = m_EditStates.find(widget);
        return it == m_EditStates.end() ? nullptr : &it->second;
    }

}
