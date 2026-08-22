#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>

namespace vae::ui::widgets {

    namespace {

        // Two panes and a divider you drag. The split is stored as a fraction on the splitter
        // rather than as a width on the pane, so it survives a window resize: a pane pinned at
        // 340px is a pane that swallows a narrow window whole.
        //
        // The divider is a plain Knob node with no behavior of its own, and the drag is handled
        // here. That keeps "where the split is" in one place — the alternative needs the handle to
        // reach up to its parent on every mouse move to ask what it is allowed to do.
        class SplitterBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Splitter; }
            bool Focusable() const override { return false; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                if (!context.Enabled()) return CursorShape::Arrow;
                if (!OverHandle(context, context.host.MousePosition())) return CursorShape::Arrow;
                return Vertical(context) ? CursorShape::ResizeV : CursorShape::ResizeH;
            }

            void Arrange(WidgetContext& context) override {
                Apply(context, Fraction(context));
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                if (IsLeftPress(event)) {
                    if (!OverHandle(context, PointOf(event))) return false;
                    context.SetState(StateBit::Pressed, true);
                    context.host.Capture(context.view);
                    return true;
                }
                if (event.type == EventType::MouseMoved
                    && context.host.Captured() == context.view) {
                    SetFromPointer(context, PointOf(event));
                    return true;
                }
                if (IsLeftRelease(event) && context.host.Captured() == context.view) {
                    context.SetState(StateBit::Pressed, false);
                    return true;
                }
                return false;
            }

            void OnCaptureLost(WidgetContext& context) override {
                context.SetState(StateBit::Pressed, false);
            }

        private:
            static bool Vertical(const WidgetContext& context) {
                return context.tree.LayoutStyleOf(context.view).axis == layout::Axis::Column;
            }

            static u32 HandleOf(const ViewTree& tree, u32 view) {
                return tree.FindRole(view, Role::Knob);
            }

            // The first child that is not the divider. Which pane moves is a choice, and moving
            // the leading one is the choice every split view makes.
            static u32 LeadingPane(const ViewTree& tree, u32 view) {
                for (u32 child : tree.At(view).children)
                    if (tree.At(child).role != Role::Knob) return child;
                return ViewTree::kInvalid;
            }

            static bool OverHandle(const WidgetContext& context, Vec2 point) {
                const u32 handle = HandleOf(context.tree, context.view);
                if (handle == ViewTree::kInvalid) return false;
                // A 6px divider is a 6px target, which is not one. Grabbing is forgiving by a few
                // pixels either side, the way every real split view is.
                Rect box = context.tree.Bounds(handle);
                const f32 grow = 3.0f;
                if (Vertical(context)) { box.pos.y -= grow; box.size.y += grow * 2.0f; }
                else                   { box.pos.x -= grow; box.size.x += grow * 2.0f; }
                return box.Contains(point);
            }

            static f32 Fraction(const WidgetContext& context) {
                return Clamped(context, context.Number(doc::Prop::Value, 0.5f));
            }

            static f32 Clamped(const WidgetContext& context, f32 t) {
                const f32 min = context.Number(doc::Prop::MinValue, 0.1f);
                const f32 max = context.Number(doc::Prop::MaxValue, 0.9f);
                return std::clamp(t, std::min(min, max), std::max(min, max));
            }

            static void Apply(WidgetContext& context, f32 t) {
                const u32 pane = LeadingPane(context.tree, context.view);
                if (pane == ViewTree::kInvalid) return;
                layout::LayoutStyle style = context.tree.LayoutStyleOf(pane);
                (Vertical(context) ? style.height : style.width) = layout::Size::Percent(t);
                context.tree.SetLayoutStyle(pane, style);
            }

            void SetFromPointer(WidgetContext& context, Vec2 point) {
                const Rect box = context.Bounds();
                const bool vertical = Vertical(context);
                const f32 span = std::max(vertical ? box.size.y : box.size.x, 1.0f);
                const f32 from = vertical ? box.Top() : box.Left();
                Commit(context, Clamped(context, ((vertical ? point.y : point.x) - from) / span));
            }

            void Commit(WidgetContext& context, f32 t) {
                if (std::abs(t - context.Number(doc::Prop::Value, 0.5f)) < 1e-4f) return;
                context.Set(doc::Prop::Value, t);
                Apply(context, t);
                Fire(context, ActionKind::ValueChanged, doc::Value{ t });
            }
        };

    }

    Scope<Behavior> MakeSplitter() { return CreateScope<SplitterBehavior>(); }

}
