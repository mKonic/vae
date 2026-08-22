#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

namespace vae::ui::widgets {

    namespace {

        // Overlay content swallows pointer events so a click inside a dialog is not also a click
        // outside it. Without this the host's dismiss-on-outside-click would fire on every press.
        class SwallowBehavior : public Behavior {
        public:
            explicit SwallowBehavior(Role role) : m_Role(role) {}
            Role Kind() const override { return m_Role; }
            bool Focusable() const override { return false; }

            bool OnEvent(WidgetContext&, const Event& event) override {
                return event.IsMouse() && event.type != EventType::MouseScrolled;
            }

        private:
            Role m_Role;
        };

        // The dimmed backdrop behind a modal. Clicking it dismisses, which is the one thing that
        // makes it more than a rectangle.
        class ScrimBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Scrim; }
            bool Focusable() const override { return false; }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (!IsLeftRelease(event)) return event.IsMouse();
                const WidgetId owner = context.host.OverlayOwnerOf(context.tree);
                context.host.Emit({ ActionKind::Dismissed, owner.node, owner.instance,
                                    context.Self().name, {} });
                context.host.CloseTopOverlay();
                return true;
            }
        };

        // A toast dismisses on click as well as on its own timer — waiting out a notification you
        // have already read is a small, constant annoyance.
        class ToastBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Toast; }
            bool Focusable() const override { return false; }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (!IsLeftRelease(event)) return event.IsMouse();
                const WidgetId owner = context.host.OverlayOwnerOf(context.tree);
                context.host.Emit({ ActionKind::Dismissed, owner.node, owner.instance,
                                    context.Self().name, {} });
                if (owner.Valid()) context.host.CloseOverlay(owner);
                return true;
            }
        };

        // Opened by waiting rather than by clicking, which is the whole of what a tooltip is.
        // Hover is tested against the trigger's own box instead of StateBit::Hovered, because the
        // thing being explained usually *is* a widget — hovering a button inside a tooltip wrapper
        // makes the button the hovered widget, and the wrapper never hears about it.
        class TooltipBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Tooltip; }
            bool Focusable() const override { return false; }

            void Sync(WidgetContext& context) override {
                const u32 bubble = context.tree.FindRole(context.view, Role::Content);
                if (bubble != ViewTree::kInvalid) context.tree.SetRuntimeVisible(bubble, false);
                context.SetState(StateBit::Open, context.host.HasOverlay(context.Id()));
            }

            void OnTick(WidgetContext& context, f32 dt) override {
                const bool inside = context.Bounds().Contains(context.host.MousePosition());
                const bool open = context.host.HasOverlay(context.Id());

                if (!inside) {
                    m_Dwell = 0.0f;
                    if (!open) return;
                    context.host.CloseOverlay(context.Id());
                    context.SetState(StateBit::Open, false);
                    return;
                }
                if (open || !context.Enabled()) return;

                m_Dwell += dt;
                if (m_Dwell < context.Number(doc::Prop::Duration, 0.45f)) return;

                const u32 bubble = context.tree.FindRole(context.view, Role::Content);
                if (bubble == ViewTree::kInvalid) return;
                context.host.OpenOverlay(context.Id(), context.tree.At(bubble).sourceId, false,
                                         context.Bounds());
                context.SetState(StateBit::Open, true);
            }

        private:
            f32 m_Dwell = 0.0f;
        };

        // The same menu a dropdown opens, opened by the right button at the pointer instead of
        // below a control. The items are ordinary DropdownItems, so choosing one closes the menu
        // and reports the index without a second kind of menu item existing.
        class ContextMenuBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::ContextMenu; }
            bool Focusable() const override { return false; }

            void Sync(WidgetContext& context) override {
                const u32 menu = context.tree.FindRole(context.view, Role::Content);
                if (menu != ViewTree::kInvalid) context.tree.SetRuntimeVisible(menu, false);
                context.SetState(StateBit::Open, context.host.HasOverlay(context.Id()));
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (!context.Enabled()) return false;
                if (event.type != EventType::MouseButtonPressed
                    || event.button.button != Mouse::Right) return false;

                const u32 menu = context.tree.FindRole(context.view, Role::Content);
                if (menu == ViewTree::kInvalid) return false;

                if (context.host.HasOverlay(context.Id())) context.host.CloseOverlay(context.Id());
                // A zero-height anchor whose bottom is the pointer: the popover placement rule
                // then puts the menu's corner exactly where the click was.
                const Vec2 at{ event.button.x, event.button.y };
                context.host.OpenOverlay(context.Id(), context.tree.At(menu).sourceId, false,
                                         Rect{ { at.x, at.y - 4.0f }, { 0.0f, 0.0f } });
                context.SetState(StateBit::Open, true);
                return true;
            }
        };

    }

    Scope<Behavior> MakeModal()   { return CreateScope<SwallowBehavior>(Role::Modal); }
    Scope<Behavior> MakePopover() { return CreateScope<SwallowBehavior>(Role::Popover); }
    Scope<Behavior> MakeScrim()   { return CreateScope<ScrimBehavior>(); }
    Scope<Behavior> MakeToast()   { return CreateScope<ToastBehavior>(); }
    Scope<Behavior> MakeTooltip() { return CreateScope<TooltipBehavior>(); }
    Scope<Behavior> MakeContextMenu() { return CreateScope<ContextMenuBehavior>(); }

}
