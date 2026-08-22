#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

namespace vae::ui::widgets {

    namespace {

        // Shows exactly one of its children, chosen by name. Screen switching is a visibility
        // decision and nothing more: the screens stay in the document, so a designer can open any
        // of them without the router having to be "run".
        class RouterBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Router; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::Hand : CursorShape::NotAllowed;
            }
            bool Focusable() const override { return false; }

            void Sync(WidgetContext& context) override {
                ViewTree& tree = context.tree;
                const auto& children = context.Self().children;
                if (children.empty()) return;

                const std::string route = context.Str(doc::Prop::Route);
                u32 active = ViewTree::kInvalid;
                for (u32 child : children) {
                    const std::string name = tree.At(child).name;
                    const std::string own = tree.Str(child, doc::Prop::Route);
                    if (!route.empty() && (name == route || own == route)) { active = child; break; }
                }
                // No route, or one naming a screen that is not here: the first child stands in, so
                // a router is never a blank rectangle. An unset route is then written back, so the
                // history has a real screen to return to rather than an empty string.
                if (active == ViewTree::kInvalid) {
                    active = children.front();
                    if (route.empty() && !tree.At(active).name.empty())
                        context.Set(doc::Prop::Route, tree.At(active).name);
                }

                for (u32 child : children) tree.SetRuntimeVisible(child, child == active);
            }
        };

    }

    Scope<Behavior> MakeRouter() { return CreateScope<RouterBehavior>(); }

}
