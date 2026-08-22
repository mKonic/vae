#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>
#include <cmath>

namespace vae::ui::widgets {

    namespace {

        // A slider you cannot grab. Same three properties, same filled child — the difference is
        // entirely that nothing here reads input, which is also why it is a separate role: a
        // progress bar that can be dragged is a bug report waiting to happen.
        class ProgressBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Progress; }
            bool Focusable() const override { return false; }

            CursorShape CursorOver(const WidgetContext&) const override {
                return CursorShape::Arrow;
            }

            void Arrange(WidgetContext& context) override {
                // A ring of dots rather than a bar. Same fact — it does not know how far along it
                // is — drawn the way people expect that fact to be drawn next to a sentence. Which
                // one it is, is which parts the component was built out of, not a second widget.
                if (const auto dots = context.tree.FindAllRoles(context.view, Role::Indicator);
                    !dots.empty()) {
                    Spin(context, dots);
                    return;
                }

                const u32 fill = context.tree.FindRole(context.view, Role::Fill);
                if (fill == ViewTree::kInvalid) return;

                if (Cycle(context) > 0.0f) { Travel(context, fill); return; }

                const f32 min = context.Number(doc::Prop::MinValue, 0.0f);
                const f32 max = context.Number(doc::Prop::MaxValue, 1.0f);
                const f32 span = max - min;
                const f32 t = std::abs(span) < 1e-6f
                            ? 0.0f
                            : std::clamp((context.Number(doc::Prop::Value, 0.0f) - min) / span,
                                         0.0f, 1.0f);

                const bool vertical = context.tree.LayoutStyleOf(context.view).axis
                                    == layout::Axis::Column;
                layout::LayoutStyle style = context.tree.LayoutStyleOf(fill);
                (vertical ? style.height : style.width) = layout::Size::Percent(t);
                context.tree.SetLayoutStyle(fill, style);
            }

            void OnTick(WidgetContext& context, f32 dt) override {
                const f32 cycle = Cycle(context);
                if (cycle <= 0.0f) return;
                (void)0;
                m_Phase = std::fmod(m_Phase + dt / cycle, 1.0f);
                // Nothing here is a transition — no state changed and nothing is easing — so the
                // frame loop has to be told, or an idle app stops the one thing that should move.
                context.host.RequestAnimation();
            }

        private:
            // Seconds per lap. A progress bar with a cycle time is one that does not know how far
            // along it is: that is what a spinner is, and it is a property rather than a separate
            // widget because the two swap places the moment a total becomes known.
            static f32 Cycle(const WidgetContext& context) {
                return context.Number(doc::Prop::Duration, 0.0f);
            }

            void Travel(WidgetContext& context, u32 fill) const {
                const f32 span = 0.3f;
                const f32 width = context.Bounds().size.x;

                // A travelling bar is placed, not stacked. The component authors a row because
                // that is what a determinate bar is; switching here rather than there means one
                // component covers both and a designer does not have to know which is which.
                layout::LayoutStyle track = context.tree.LayoutStyleOf(context.view);
                if (track.mode != layout::LayoutMode::Absolute) {
                    track.mode = layout::LayoutMode::Absolute;
                    context.tree.SetLayoutStyle(context.view, track);
                }

                layout::LayoutStyle style = context.tree.LayoutStyleOf(fill);
                style.width = layout::Size::Percent(span);
                style.constraintX = layout::Constraint::Start;
                // From just off the left edge to just off the right, and the parent clips.
                style.offsetStart.x = width * (m_Phase * (1.0f + span) - span);
                context.tree.SetLayoutStyle(fill, style);
            }

            // Eight dots around a circle, each brighter the more recently the sweep passed it.
            // Positions are set here rather than authored, so the ring follows whatever size the
            // thing ends up and a designer can drop it anywhere without doing trigonometry.
            void Spin(WidgetContext& context, const std::vector<u32>& dots) const {
                const Vec2 box = context.Bounds().size;
                const f32 diameter = std::min(box.x, box.y);
                if (diameter < 4.0f) return;

                const f32 dot = std::max(diameter * 0.18f, 2.0f);
                const f32 radius = (diameter - dot) * 0.5f;
                const auto count = static_cast<f32>(dots.size());

                for (std::size_t i = 0; i < dots.size(); ++i) {
                    const f32 turn = static_cast<f32>(i) / count;
                    const f32 angle = turn * 6.2831853f - 1.5707963f;   // twelve o'clock first

                    layout::LayoutStyle style = context.tree.LayoutStyleOf(dots[i]);
                    style.width = layout::Size::Px(dot);
                    style.height = layout::Size::Px(dot);
                    style.constraintX = layout::Constraint::Start;
                    style.constraintY = layout::Constraint::Start;
                    style.offsetStart = { box.x * 0.5f - dot * 0.5f + std::cos(angle) * radius,
                                          box.y * 0.5f - dot * 0.5f + std::sin(angle) * radius };
                    context.tree.SetLayoutStyle(dots[i], style);

                    // How long ago the sweep went past, as a fraction of a lap.
                    f32 behind = m_Phase - turn;
                    behind -= std::floor(behind);
                    context.tree.SetViewPropLocal(dots[i], doc::Prop::Opacity,
                                                  0.15f + 0.85f * (1.0f - behind));
                }
            }

            f32 m_Phase = 0.0f;
        };

    }

    Scope<Behavior> MakeProgress() { return CreateScope<ProgressBehavior>(); }

}
