#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>
#include <cmath>

namespace vae::ui::widgets {

    namespace {

        // Prev · 1 2 3 · Next. The page number lives on the pagination as Value, one-based, because
        // that is the number the user is looking at and the number a script wants to send to a
        // server. The pages themselves are Tab-roled children, which gets them the selected styling
        // every other selectable thing in the library already has.
        //
        // Prev and Next are plain frames the behavior hit-tests, not Buttons: a Button would eat
        // the click and report only that it was clicked, and the arrow has to change the page.
        class PaginationBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Pagination; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                if (!context.Enabled()) return CursorShape::NotAllowed;
                const Vec2 at = context.host.MousePosition();
                if (Step(context, at) != 0 || PageAt(context, at) >= 0) return CursorShape::Hand;
                return CursorShape::Arrow;
            }

            void Sync(WidgetContext& context) override { Apply(context); }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                if (IsLeftPress(event)) {
                    const Vec2 at = PointOf(event);
                    return Step(context, at) != 0 || PageAt(context, at) >= 0;
                }
                if (IsLeftRelease(event)) {
                    const Vec2 at = PointOf(event);
                    if (const i32 step = Step(context, at); step != 0) {
                        GoTo(context, Page(context) + step);
                        return true;
                    }
                    if (const i32 page = PageAt(context, at); page >= 0) {
                        GoTo(context, page + 1);
                        return true;
                    }
                    return false;
                }
                if (event.type == EventType::KeyPressed) {
                    switch (event.key.code) {
                        case Key::Left:  GoTo(context, Page(context) - 1); return true;
                        case Key::Right: GoTo(context, Page(context) + 1); return true;
                        case Key::Home:  GoTo(context, 1); return true;
                        case Key::End:   GoTo(context, Last(context)); return true;
                        default: break;
                    }
                }
                return false;
            }

        private:
            static i32 Page(const WidgetContext& context) {
                return static_cast<i32>(std::lround(context.Number(doc::Prop::Value, 1.0f)));
            }
            // The last page. Zero or unset means "as many as there are page buttons", so a
            // pagination someone drew four pages into needs no second number kept in step.
            static i32 Last(const WidgetContext& context) {
                const i32 stated = static_cast<i32>(std::lround(context.Number(doc::Prop::MaxValue, 0.0f)));
                if (stated > 0) return stated;
                const auto pages = context.tree.FindAllRoles(context.view, Role::Tab);
                return std::max(static_cast<i32>(pages.size()), 1);
            }

            static u32 NamedChild(const ViewTree& tree, u32 view, std::string_view name) {
                for (u32 child : tree.At(view).children)
                    if (tree.At(child).name == name) return child;
                return ViewTree::kInvalid;
            }

            // -1 for Prev, +1 for Next, 0 for neither.
            static i32 Step(const WidgetContext& context, Vec2 at) {
                const ViewTree& tree = context.tree;
                const u32 prev = NamedChild(tree, context.view, "Prev");
                const u32 next = NamedChild(tree, context.view, "Next");
                if (prev != ViewTree::kInvalid && tree.Bounds(prev).Contains(at)) return -1;
                if (next != ViewTree::kInvalid && tree.Bounds(next).Contains(at)) return 1;
                return 0;
            }

            static i32 PageAt(const WidgetContext& context, Vec2 at) {
                const auto pages = context.tree.FindAllRoles(context.view, Role::Tab);
                for (u32 i = 0; i < pages.size(); ++i)
                    if (context.tree.Bounds(pages[i]).Contains(at)) return static_cast<i32>(i);
                return -1;
            }

            static void Apply(WidgetContext& context) {
                ViewTree& tree = context.tree;
                const i32 page = Page(context);
                const auto pages = tree.FindAllRoles(context.view, Role::Tab);
                for (u32 i = 0; i < pages.size(); ++i)
                    tree.SetState(pages[i], StateBit::Selected, static_cast<i32>(i) + 1 == page);

                // The ends grey out rather than disappearing: a control that moves under the
                // pointer between clicks is a control you misclick.
                const u32 prev = NamedChild(tree, context.view, "Prev");
                const u32 next = NamedChild(tree, context.view, "Next");
                if (prev != ViewTree::kInvalid) tree.SetState(prev, StateBit::Disabled, page <= 1);
                if (next != ViewTree::kInvalid)
                    tree.SetState(next, StateBit::Disabled, page >= Last(context));
            }

            static void GoTo(WidgetContext& context, i32 page) {
                page = std::clamp(page, 1, Last(context));
                if (page == Page(context)) return;
                context.Set(doc::Prop::Value, static_cast<f32>(page));
                Apply(context);
                Fire(context, ActionKind::ValueChanged, doc::Value{ static_cast<f32>(page) });
            }
        };

    }

    Scope<Behavior> MakePagination() { return CreateScope<PaginationBehavior>(); }

}
