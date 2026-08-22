#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

namespace vae::ui::widgets {

    namespace {

        // Press-drag-release, with the press held only while the pointer stays inside. Releasing
        // outside cancels — the standard escape hatch for a click you changed your mind about, and
        // the reason a button captures the pointer instead of just watching for a release.
        class ButtonBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Button; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::Hand : CursorShape::NotAllowed;
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                if (IsLeftPress(event)) {
                    context.SetState(StateBit::Pressed, true);
                    context.host.Capture(context.view);
                    return true;
                }

                if (event.type == EventType::MouseMoved && context.host.Captured() == context.view) {
                    context.SetState(StateBit::Pressed, context.Bounds().Contains(PointOf(event)));
                    return true;
                }

                if (IsLeftRelease(event)) {
                    const bool wasPressed = HasState(context.Self().state, StateBit::Pressed);
                    context.SetState(StateBit::Pressed, false);
                    if (wasPressed && context.Bounds().Contains(PointOf(event)))
                        Fire(context, ActionKind::Clicked);
                    return true;
                }

                if (event.type == EventType::KeyPressed
                    && (event.key.code == Key::Space || event.key.code == Key::Enter)) {
                    Fire(context, ActionKind::Clicked);
                    return true;
                }
                return false;
            }

            void OnCaptureLost(WidgetContext& context) override {
                context.SetState(StateBit::Pressed, false);
            }
        };

    }

    Scope<Behavior> MakeButton() { return CreateScope<ButtonBehavior>(); }

}
