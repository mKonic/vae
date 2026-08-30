#pragma once

#include "vae/ui/Behavior.h"

#include <map>

namespace vae::ui {

    // Clipboard access, injected rather than linked. The desktop backend installs a GLFW-backed
    // one; a headless test installs a std::string, and Ctrl+C/Ctrl+V are then testable without a
    // display server.
    class Clipboard {
    public:
        virtual ~Clipboard() = default;
        virtual void SetText(const std::string& text) = 0;
        virtual std::string GetText() const = 0;
    };

    class MemoryClipboard final : public Clipboard {
    public:
        void SetText(const std::string& text) override { m_Text = text; }
        std::string GetText() const override { return m_Text; }
    private:
        std::string m_Text;
    };

    // Transient state a text field keeps between events. Lives here rather than in the behavior
    // because the view tree is rebuilt on any document change, and a caret must survive one.
    struct TextEditState {
        std::size_t caret = 0;
        std::size_t anchor = 0;
        f32 blink = 0.0f;
        bool HasSelection() const { return caret != anchor; }
        std::size_t Begin() const { return caret < anchor ? caret : anchor; }
        std::size_t End()   const { return caret < anchor ? anchor : caret; }
    };

    // Routes input into a view tree and owns everything that is true of the UI as a whole rather
    // than of one widget: what is hovered, focused, captured, and what is floating above.
    class UiHost {
    public:
        UiHost();
        ~UiHost();

        void SetDocument(doc::Document& document, Uuid root);
        void SetClipboard(Scope<Clipboard> clipboard);
        Clipboard& GetClipboard() { return *m_Clipboard; }

        // Rebuilds if the document changed, then lays out. Call once per frame before painting.
        void Update(Vec2 available, f32 dt);
        // Whether a transition is still running. An idle main loop that sleeps until the next input
        // has to be told about the one thing on screen that moves without any.
        bool Animating() const { return m_Animating; }
        // Whether this host owes another frame: something is moving, something asked for one, or a
        // script changed the tree after it was laid out. What an idle loop asks before going back
        // to sleep.
        bool NeedsFrame() const;

        // A stamp for everything a screen reader would notice: what every tree says, which widget
        // has focus, and where the carets are. Derived from what the frame actually did rather
        // than maintained here, so the only place that has to remember to move it is ViewTree.
        u64 AccessibilityStamp() const;
        // A behavior saying "I am moving, keep drawing". A spinner is not a transition — nothing
        // changed state and nothing is easing — so without this the frame loop sleeps and the one
        // widget whose whole job is to move stops moving. Cleared and re-asked every frame.
        void RequestAnimation() { m_AnimationRequested = true; }
        void SetMotion(ViewTree::Motion motion);
        void Paint(PaintContext& context);

        bool Dispatch(const Event& event);
        bool DispatchAll(std::initializer_list<Event> events);

        // --- pointer and focus ---------------------------------------------------------------
        Vec2 MousePosition() const { return m_Mouse; }
        u32  Hovered() const { return m_Hovered; }
        u32  Focused() const { return m_Focused; }
        u32  Captured() const { return m_Captured; }

        void Focus(u32 view);
        void FocusNext(bool backwards = false);
        void Capture(u32 view);
        void ReleaseCapture();

        CursorShape Cursor() const { return m_Cursor; }
        void RequestCursor(CursorShape shape) { m_Cursor = shape; }

        // --- overlays --------------------------------------------------------------------------
        // A modal, popover or toast: its own view tree over the main one. Anchored overlays follow
        // a view; modal ones swallow everything beneath them.
        struct Overlay {
            WidgetId owner;                  // the widget that opened it
            Uuid contentRoot = Uuid::Invalid();
            Scope<ViewTree> tree;
            bool modal = false;
            bool dismissOnOutsideClick = true;
            // Whether the host draws the dimming behind it. A Modal *widget* has a scrim node of
            // its own that a designer can restyle; a screen presented as a modal has no such node,
            // so the host supplies one from the theme.
            bool scrim = false;
            Rect anchor{};                   // for popovers, in absolute space
            f32  timeToLive = 0.0f;          // > 0 for toasts
        };

        void OpenOverlay(WidgetId owner, Uuid contentRoot, bool modal,
                         Rect anchor = {}, f32 timeToLive = 0.0f);
        void CloseOverlay(WidgetId owner);
        void CloseTopOverlay();
        bool HasOverlay(WidgetId owner) const;
        std::size_t OverlayCount() const { return m_Overlays.size(); }
        const Overlay& OverlayAt(std::size_t index) const { return *m_Overlays[index]; }

        // --- data sources ----------------------------------------------------------------------
        // A virtualized list never materializes a node per row: the template node is styled by the
        // designer and the rows come from here, so a million-row table costs one template.
        class ListDataSource {
        public:
            virtual ~ListDataSource() = default;
            virtual u32 Count() const = 0;
            virtual std::string Cell(u32 row, u32 column) const = 0;
        };
        void SetDataSource(WidgetId widget, Ref<ListDataSource> source);
        ListDataSource* DataSource(WidgetId widget) const;

        // --- screens -----------------------------------------------------------------------------
        // Screen-to-screen navigation, as opposed to the Router widget below, which routes inside
        // one screen. Which one a script means is decided by the name: a screen wins.
        //
        // A page replaces what is on screen and pushes it onto the back stack. Every other kind is
        // presented over it — the screen underneath stays exactly where it was, which is what makes
        // "close the dialog and carry on" work without the app rebuilding itself.
        bool GoToScreen(std::string_view name);
        bool GoToScreen(Uuid screen);
        // Closes the top overlay if there is one, otherwise pops the page stack. Returns false when
        // there is nowhere to go, which is what a hardware back button needs to know.
        bool GoBack();
        // Declared navigation is queued, not immediate. A click both fires an action and may lead
        // somewhere, and a script has to see the click before the screen it happened on goes away —
        // otherwise "remember which row was picked" loses to "open the detail" every time.
        void RequestNavigation(std::string where);
        bool ApplyNavigation();
        bool NavigationPending() const { return !m_PendingNavigation.empty(); }
        Uuid CurrentScreen() const { return m_RootId; }
        std::string CurrentScreenName() const;
        std::size_t ScreenDepth() const { return m_ScreenStack.size(); }
        bool HasScreen(std::string_view name) const;

        // --- routing ---------------------------------------------------------------------------
        void Navigate(WidgetId router, std::string route);
        bool Back(WidgetId router);
        std::string Route(WidgetId router) const;
        std::size_t HistoryDepth(WidgetId router) const;

        // Which widget opened the overlay this tree belongs to, so a menu item can reach the
        // control that spawned it across the tree boundary.
        WidgetId OverlayOwnerOf(const ViewTree& tree) const;
        // The other direction: the tree an owner's overlay is showing, or null. A combobox filters
        // the menu it opened, and the menu lives in a tree the owner is not in.
        ViewTree* OverlayTreeOf(WidgetId owner);

        // --- actions ---------------------------------------------------------------------------
        void Emit(Action action);
        const std::vector<Action>& Actions() const { return m_Actions; }
        void ClearActions() { m_Actions.clear(); }
        // Consumes and returns the queue, which is what a frame loop actually wants.
        std::vector<Action> TakeActions();
        bool Fired(ActionKind kind, std::string_view name = {}) const;

        // --- text editing ----------------------------------------------------------------------
        TextEditState& EditState(WidgetId widget) { return m_EditStates[widget]; }
        const TextEditState* FindEditState(WidgetId widget) const;

        ViewTree& Tree() { return *m_Tree; }
        const ViewTree& Tree() const { return *m_Tree; }
        // The tree an event would land in: the topmost overlay, or the main tree.
        ViewTree& ActiveTree();
        const ViewTree& ActiveTree() const;
        // Every tree currently on screen: the main one, then each overlay above it in order.
        // Anything that has to see the whole screen — the script runtime, a debugger — walks this
        // rather than reaching for Tree() and quietly missing whatever is presented over it.
        std::vector<ViewTree*> Trees();

        f32 Time() const { return m_Time; }
        void MarkDirty() { m_Dirty = true; }

    private:
        struct Target { ViewTree* tree = nullptr; u32 view = ViewTree::kInvalid; };

        Target Pick(Vec2 point);
        Target Locate(WidgetId id);
        void   Write(WidgetId widget, doc::Prop prop, doc::Value value);
        bool   Bubble(ViewTree& tree, u32 from, const Event& event);
        void   UpdateHover(Vec2 point);
        void   CollectFocusables(ViewTree& tree, u32 view, std::vector<u32>& out) const;
        void   Rebuild();
        // Lay out, let the behaviors place the parts they own, and lay out again only if that
        // moved anything.
        void   Settle();
        void   LayoutAll();
        void   ArrangeAll();
        void   SyncBehaviors(ViewTree& tree);
        bool   NeedsRowRebuild() const;
        void   TickTree(ViewTree& tree, f32 dt);
        void   PaintScrim(draw::DrawList& list) const;

        doc::Document* m_Document = nullptr;
        Uuid m_RootId = Uuid::Invalid();
        Scope<ViewTree> m_Tree;
        std::vector<Scope<Overlay>> m_Overlays;
        Scope<Clipboard> m_Clipboard;

        u32 m_Observer = 0;
        // Where Back goes. Ids, not indices: a screen can be deleted while it is on the stack.
        std::vector<Uuid> m_ScreenStack;
        std::string m_PendingNavigation;
        bool m_Animating = false;
        bool m_AnimationRequested = false;
        bool m_Dirty = true;

        // Focus and capture are remembered by id as well as index: a document edit rebuilds the
        // view tree and renumbers everything, and typing must survive that.
        WidgetId m_FocusedId;
        WidgetId m_CapturedId;

        Vec2 m_Mouse{ -1.0f, -1.0f };
        Vec2 m_Available{ 0.0f, 0.0f };
        u32  m_Hovered = ViewTree::kInvalid;
        u32  m_Focused = ViewTree::kInvalid;
        u32  m_Captured = ViewTree::kInvalid;
        ViewTree* m_HoverTree = nullptr;
        ViewTree* m_FocusTree = nullptr;
        ViewTree* m_CaptureTree = nullptr;
        CursorShape m_Cursor = CursorShape::Arrow;

        std::vector<Action> m_Actions;
        std::map<WidgetId, TextEditState> m_EditStates;
        std::map<WidgetId, Ref<ListDataSource>> m_DataSources;
        std::map<WidgetId, std::vector<std::string>> m_History;
        f32 m_Time = 0.0f;
    };

}
