#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>
#include <cmath>

namespace vae::ui::widgets {

    namespace {

        class SliderBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Slider; }

            // Grabbing, not pointing: a slider is dragged along its track.
            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::ResizeH : CursorShape::NotAllowed;
            }

            void Arrange(WidgetContext& context) override { Apply(context, Value(context)); }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                if (IsLeftPress(event)) {
                    context.SetState(StateBit::Pressed, true);
                    context.host.Capture(context.view);
                    SetFromPointer(context, PointOf(event).x);
                    return true;
                }
                if (event.type == EventType::MouseMoved && context.host.Captured() == context.view) {
                    SetFromPointer(context, PointOf(event).x);
                    return true;
                }
                if (IsLeftRelease(event)) {
                    context.SetState(StateBit::Pressed, false);
                    return true;
                }
                if (event.type == EventType::KeyPressed) {
                    const f32 min = context.Number(doc::Prop::MinValue, 0.0f);
                    const f32 max = context.Number(doc::Prop::MaxValue, 1.0f);
                    // A slider with no explicit step still has to be usable from the keyboard, and
                    // 1% of the range is the increment every platform's slider settles on.
                    const f32 step = context.Number(doc::Prop::Step, 0.0f) > 0.0f
                                   ? context.Number(doc::Prop::Step, 0.0f)
                                   : (max - min) * 0.01f;
                    switch (event.key.code) {
                        case Key::Left: case Key::Down:  Commit(context, Value(context) - step); return true;
                        case Key::Right: case Key::Up:   Commit(context, Value(context) + step); return true;
                        case Key::Home:                  Commit(context, min); return true;
                        case Key::End:                   Commit(context, max); return true;
                        default: break;
                    }
                }
                return false;
            }

            void OnCaptureLost(WidgetContext& context) override {
                context.SetState(StateBit::Pressed, false);
            }

        private:
            static f32 Value(const WidgetContext& context) {
                return context.Number(doc::Prop::Value, 0.0f);
            }

            // The track is a child if the designer drew one, and the slider itself otherwise, so a
            // bare slider still works before anyone styles it.
            static Rect TrackRect(const WidgetContext& context) {
                const u32 track = context.tree.FindRole(context.view, Role::Track);
                return track == ViewTree::kInvalid ? context.Bounds() : context.tree.Bounds(track);
            }

            void SetFromPointer(WidgetContext& context, f32 x) {
                const Rect track = TrackRect(context);
                const u32 knob = context.tree.FindRole(context.view, Role::Knob);
                const f32 knobWidth = knob == ViewTree::kInvalid ? 0.0f
                                                                 : context.tree.Bounds(knob).size.x;
                // The knob's centre follows the pointer, so the usable travel is the track minus
                // the knob — without this the value saturates before the knob reaches either end.
                const f32 travel = std::max(track.size.x - knobWidth, 1.0f);
                const f32 t = std::clamp((x - track.Left() - knobWidth * 0.5f) / travel, 0.0f, 1.0f);

                const f32 min = context.Number(doc::Prop::MinValue, 0.0f);
                const f32 max = context.Number(doc::Prop::MaxValue, 1.0f);
                Commit(context, min + t * (max - min));
            }

            void Commit(WidgetContext& context, f32 value) {
                const f32 min = context.Number(doc::Prop::MinValue, 0.0f);
                const f32 max = context.Number(doc::Prop::MaxValue, 1.0f);
                const f32 step = context.Number(doc::Prop::Step, 0.0f);

                value = std::clamp(value, std::min(min, max), std::max(min, max));
                if (step > 0.0f) value = min + std::round((value - min) / step) * step;
                value = std::clamp(value, std::min(min, max), std::max(min, max));

                if (std::abs(value - Value(context)) < 1e-6f) return;
                context.Set(doc::Prop::Value, value);
                Fire(context, ActionKind::ValueChanged, doc::Value{ value });
            }

            // Moves the parts the designer drew rather than painting anything: the filled portion
            // of the track and the knob are ordinary nodes, resized and offset in the layout tree
            // and never written back to the document.
            void Apply(WidgetContext& context, f32 value) {
                const f32 min = context.Number(doc::Prop::MinValue, 0.0f);
                const f32 max = context.Number(doc::Prop::MaxValue, 1.0f);
                const f32 span = max - min;
                const f32 t = std::abs(span) < 1e-6f ? 0.0f : std::clamp((value - min) / span, 0.0f, 1.0f);

                ViewTree& tree = context.tree;
                if (const u32 fill = tree.FindRole(context.view, Role::Fill); fill != ViewTree::kInvalid) {
                    layout::LayoutStyle style = tree.LayoutStyleOf(fill);
                    style.width = layout::Size::Percent(t);
                    tree.SetLayoutStyle(fill, style);
                }
                if (const u32 knob = tree.FindRole(context.view, Role::Knob); knob != ViewTree::kInvalid) {
                    const Rect track = TrackRect(context);
                    const f32 knobWidth = tree.Bounds(knob).size.x;
                    layout::LayoutStyle style = tree.LayoutStyleOf(knob);
                    style.constraintX = layout::Constraint::Start;
                    style.offsetStart.x = t * std::max(track.size.x - knobWidth, 0.0f);
                    tree.SetLayoutStyle(knob, style);
                }
            }
        };

    }

    Scope<Behavior> MakeSlider() { return CreateScope<SliderBehavior>(); }

}
