#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>

namespace vae::ui::widgets {

    namespace {

        // Checkbox, switch and radio differ in exactly two ways: whether activating twice turns
        // them back off, and whether turning one on turns its group mates off. Everything else —
        // the tick's visibility, the state flag, the emitted action — is shared.
        class ToggleBehavior : public Behavior {
        public:
            explicit ToggleBehavior(Role role, bool exclusive) : m_Role(role), m_Exclusive(exclusive) {}

            Role Kind() const override { return m_Role; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::Hand : CursorShape::NotAllowed;
            }

            void Sync(WidgetContext& context) override {
                Apply(context, context.Flag(doc::Prop::Checked));
            }

            void Arrange(WidgetContext& context) override {
                SlideKnob(context, context.Flag(doc::Prop::Checked));
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                if (IsLeftPress(event)) {
                    context.SetState(StateBit::Pressed, true);
                    context.host.Capture(context.view);
                    return true;
                }
                if (IsLeftRelease(event)) {
                    const bool wasPressed = HasState(context.Self().state, StateBit::Pressed);
                    context.SetState(StateBit::Pressed, false);
                    if (wasPressed && context.Bounds().Contains(PointOf(event))) Activate(context);
                    return true;
                }
                if (event.type == EventType::MouseMoved && context.host.Captured() == context.view) {
                    context.SetState(StateBit::Pressed, context.Bounds().Contains(PointOf(event)));
                    return true;
                }
                if (event.type == EventType::KeyPressed
                    && (event.key.code == Key::Space || event.key.code == Key::Enter)) {
                    Activate(context);
                    return true;
                }
                return false;
            }

            void OnCaptureLost(WidgetContext& context) override {
                context.SetState(StateBit::Pressed, false);
            }

        private:
            void Activate(WidgetContext& context) {
                const bool current = context.Flag(doc::Prop::Checked);
                // A radio is not a toggle: clicking the selected one keeps it selected, because the
                // group would otherwise be left with nothing chosen and no way back.
                const bool next = m_Exclusive ? true : !current;
                if (m_Exclusive && current) return;

                context.Set(doc::Prop::Checked, next);
                Apply(context, next);
                SlideKnob(context, next);
                if (m_Exclusive) ClearGroup(context);
                Fire(context, ActionKind::ValueChanged, doc::Value{ next });
            }

            void Apply(WidgetContext& context, bool checked) {
                context.SetState(StateBit::Checked, checked);
                // The tick, the dot, the switch knob: a real node the designer drew, shown or
                // hidden rather than painted by the engine.
                const u32 indicator = context.tree.FindRole(context.view, Role::Indicator);
                if (indicator != ViewTree::kInvalid)
                    context.tree.SetRuntimeVisible(indicator, checked);
            }

            // A switch does not show and hide its knob, it slides it. Same behavior, same state
            // flag, one different visual consequence — and one that needs real boxes, so it runs
            // after layout rather than in Sync.
            void SlideKnob(WidgetContext& context, bool checked) {
                const u32 knob = context.tree.FindRole(context.view, Role::Knob);
                const u32 track = context.tree.FindRole(context.view, Role::Track);
                if (knob == ViewTree::kInvalid || track == ViewTree::kInvalid) return;
                layout::LayoutStyle style = context.tree.LayoutStyleOf(knob);
                const f32 inset = style.offsetEnd.x > 0.0f ? style.offsetEnd.x : 2.0f;
                const f32 travel = std::max(context.tree.Bounds(track).size.x
                                            - context.tree.Bounds(knob).size.x - inset * 2.0f, 0.0f);
                style.constraintX = layout::Constraint::Start;
                style.offsetStart.x = inset + (checked ? travel : 0.0f);
                context.tree.SetLayoutStyle(knob, style);
            }

            void ClearGroup(WidgetContext& context) {
                const std::string group = context.Str(doc::Prop::Group);
                ViewTree& tree = context.tree;
                for (u32 peer : tree.FindAllRoles(tree.Root(), Role::Radio)) {
                    if (peer == context.view) continue;
                    if (tree.Str(peer, doc::Prop::Group) != group) continue;
                    if (!tree.Flag(peer, doc::Prop::Checked)) continue;

                    tree.SetViewProp(peer, doc::Prop::Checked, doc::Value{ false });
                    tree.SetState(peer, StateBit::Checked, false);
                    const u32 indicator = tree.FindRole(peer, Role::Indicator);
                    if (indicator != ViewTree::kInvalid) tree.SetRuntimeVisible(indicator, false);
                }
            }

            Role m_Role;
            bool m_Exclusive;
        };

    }

    Scope<Behavior> MakeCheckbox() { return CreateScope<ToggleBehavior>(Role::Checkbox, false); }
    Scope<Behavior> MakeSwitch()   { return CreateScope<ToggleBehavior>(Role::Switch, false); }
    Scope<Behavior> MakeRadio()    { return CreateScope<ToggleBehavior>(Role::Radio, true); }

}
