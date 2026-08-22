#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>

namespace vae::ui::widgets {

    namespace {

        // A dropdown's menu is an ordinary subtree of the dropdown, marked Content. Closed, it is
        // hidden and takes no space; open, the host builds a second view tree over that same node
        // and floats it. So the menu a designer styles and the menu that appears are one node.
        //
        // Two roles, one behavior, one difference: a *select* wears the choice as its own label, a
        // *menu* does not. That is the whole distinction the web draws between `select` and
        // `dropdown-menu`, and conflating them is how a File menu ends up titled "Save As".
        class DropdownBehavior final : public Behavior {
        public:
            explicit DropdownBehavior(Role role, bool wearsChoice)
                : m_Role(role), m_WearsChoice(wearsChoice) {}

            Role Kind() const override { return m_Role; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::Hand : CursorShape::NotAllowed;
            }

            void Sync(WidgetContext& context) override {
                const u32 menu = context.tree.FindRole(context.view, Role::Content);
                if (menu != ViewTree::kInvalid) context.tree.SetRuntimeVisible(menu, false);
                context.SetState(StateBit::Open, context.host.HasOverlay(context.Id()));
                if (m_WearsChoice) ShowSelection(context);
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                const bool activate = IsLeftPress(event)
                    || (event.type == EventType::KeyPressed
                        && (event.key.code == Key::Enter || event.key.code == Key::Space
                            || event.key.code == Key::Down));
                if (!activate) return event.type == EventType::MouseButtonReleased;

                Toggle(context);
                return true;
            }

        private:
            void Toggle(WidgetContext& context) {
                const WidgetId self = context.Id();
                if (context.host.HasOverlay(self)) {
                    context.host.CloseOverlay(self);
                    context.SetState(StateBit::Open, false);
                    return;
                }
                const u32 menu = context.tree.FindRole(context.view, Role::Content);
                if (menu == ViewTree::kInvalid) return;
                context.host.OpenOverlay(self, context.tree.At(menu).sourceId, false, context.Bounds());
                context.SetState(StateBit::Open, true);
            }

            void ShowSelection(WidgetContext& context) {
                const u32 menu = context.tree.FindRole(context.view, Role::Content);
                if (menu == ViewTree::kInvalid) return;
                const auto items = context.tree.FindAllRoles(menu, Role::DropdownItem);
                const auto index = static_cast<i32>(context.Number(doc::Prop::SelectedIndex, -1.0f));
                if (index < 0 || static_cast<std::size_t>(index) >= items.size()) return;

                const u32 label = LabelOf(context.tree, context.view);
                const u32 itemLabel = LabelOf(context.tree, items[static_cast<std::size_t>(index)]);
                if (label == ViewTree::kInvalid || itemLabel == ViewTree::kInvalid) return;
                context.tree.At(label).props.Set(
                    doc::Prop::Text, doc::Value{ context.tree.Str(itemLabel, doc::Prop::Text) });
            }

            Role m_Role;
            bool m_WearsChoice;
        };

        class DropdownItemBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::DropdownItem; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::Hand : CursorShape::NotAllowed;
            }
            bool Focusable() const override { return false; }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;
                if (!IsLeftRelease(event) && !IsLeftPress(event)) return false;
                if (IsLeftPress(event)) return true;   // the choice lands on release, as menus do

                Choose(context);
                return true;
            }

        private:
            void Choose(WidgetContext& context) {
                ViewTree& menu = context.tree;
                // The item lives in the overlay's tree; the dropdown it belongs to does not. The
                // overlay remembers who opened it, which is the only link across that boundary.
                const WidgetId ownerId = context.host.OverlayOwnerOf(menu);
                const u32 index = IndexAmongRole(menu, menu.Root(), context.view, Role::DropdownItem);
                const std::string text = [&] {
                    const u32 label = LabelOf(menu, context.view);
                    return label == ViewTree::kInvalid ? std::string{} : menu.Str(label, doc::Prop::Text);
                }();

                ViewTree& main = context.host.Tree();
                const u32 owner = ownerId.Valid() ? main.ViewOf(ownerId) : ViewTree::kInvalid;
                if (owner != ViewTree::kInvalid) {
                    main.SetViewProp(owner, doc::Prop::SelectedIndex, static_cast<f32>(index));
                    // Only a select wears its choice. A menu's owner keeps its own name, and a
                    // context menu's owner is whatever region was right-clicked — stamping the
                    // chosen item's text over the first label in it rewrites the page rather than
                    // acting on it.
                    const u32 label = main.At(owner).role == Role::Dropdown
                                    ? LabelOf(main, owner) : ViewTree::kInvalid;
                    if (label != ViewTree::kInvalid)
                        main.At(label).props.Set(doc::Prop::Text, doc::Value{ text });
                    context.host.Emit({ ActionKind::SelectionChanged, ownerId.node, ownerId.instance,
                                        main.At(owner).name, doc::Value{ static_cast<f32>(index) } });
                }
                // And the item reports itself, by name. A menu holds actions, and "Save was
                // chosen" is what a script wants to hear — not "the third item of some menu".
                // Fire also honours a declared `goTo`, so a menu item can navigate with no script.
                Fire(context, ActionKind::Clicked);
                if (ownerId.Valid()) context.host.CloseOverlay(ownerId);
            }
        };

        // Tabs hold two kinds of child: the tabs themselves and the panels they switch between,
        // matched by order. Selecting one is a document edit — which tab is open is part of the
        // design, and survives a save.
        class TabsBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Tabs; }
            bool Focusable() const override { return true; }

            void Sync(WidgetContext& context) override { Apply(context); }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (event.type != EventType::KeyPressed || !context.Enabled()) return false;

                const auto tabs = context.tree.FindAllRoles(context.view, Role::Tab);
                if (tabs.empty()) return false;
                const auto count = static_cast<i32>(tabs.size());
                const i32 current = static_cast<i32>(context.Number(doc::Prop::SelectedIndex, 0.0f));

                switch (event.key.code) {
                    case Key::Left:  Select(context, (current - 1 + count) % count); return true;
                    case Key::Right: Select(context, (current + 1) % count); return true;
                    case Key::Home:  Select(context, 0); return true;
                    case Key::End:   Select(context, count - 1); return true;
                    default: return false;
                }
            }

            static void Select(WidgetContext& context, i32 index) {
                if (static_cast<i32>(context.Number(doc::Prop::SelectedIndex, 0.0f)) == index) return;
                context.Set(doc::Prop::SelectedIndex, static_cast<f32>(index));
                Apply(context);
                Fire(context, ActionKind::SelectionChanged, doc::Value{ static_cast<f32>(index) });
            }

        private:
            static void Apply(WidgetContext& context) {
                ViewTree& tree = context.tree;
                const i32 index = static_cast<i32>(context.Number(doc::Prop::SelectedIndex, 0.0f));

                const auto tabs = tree.FindAllRoles(context.view, Role::Tab);
                for (u32 i = 0; i < tabs.size(); ++i)
                    tree.SetState(tabs[i], StateBit::Selected, static_cast<i32>(i) == index);

                const auto panels = tree.FindAllRoles(context.view, Role::Content);
                for (u32 i = 0; i < panels.size(); ++i)
                    tree.SetRuntimeVisible(panels[i], static_cast<i32>(i) == index);
            }
        };

        class TabBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Tab; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::Hand : CursorShape::NotAllowed;
            }
            bool Focusable() const override { return false; }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!IsLeftPress(event) || !context.Enabled()) return false;

                const u32 tabs = AncestorWithRole(context.tree, context.view, Role::Tabs);
                if (tabs == ViewTree::kInvalid) return false;
                const u32 index = IndexAmongRole(context.tree, tabs, context.view, Role::Tab);
                if (index == UINT32_MAX) return false;

                WidgetContext owner{ context.tree, context.host, tabs };
                TabsBehavior::Select(owner, static_cast<i32>(index));
                return true;
            }
        };

    }

    Scope<Behavior> MakeDropdown() { return CreateScope<DropdownBehavior>(Role::Dropdown, true); }
    Scope<Behavior> MakeMenu()     { return CreateScope<DropdownBehavior>(Role::Menu, false); }
    Scope<Behavior> MakeDropdownItem() { return CreateScope<DropdownItemBehavior>(); }
    Scope<Behavior> MakeTabs()         { return CreateScope<TabsBehavior>(); }
    Scope<Behavior> MakeTab()          { return CreateScope<TabBehavior>(); }

}
