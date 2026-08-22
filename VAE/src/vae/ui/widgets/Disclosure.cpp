#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

namespace vae::ui::widgets {

    namespace {

        // A section that folds away. The header is an ordinary frame rather than a Button, because
        // a Button would swallow the click and fire Clicked instead of opening anything — the row
        // you press and the thing that opens have to be the same widget.
        //
        // What "open" looks like is not this behavior's business: it sets StateBit::Open on the
        // root, and EffectiveState carries that down to the chevron, so the arrow that turns is a
        // `open:text` on a text node the designer can edit.
        class CollapsibleBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Collapsible; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                if (!context.Enabled()) return CursorShape::NotAllowed;
                return OverHeader(context, context.host.MousePosition()) ? CursorShape::Hand
                                                                         : CursorShape::Arrow;
            }

            void Sync(WidgetContext& context) override {
                Apply(context.tree, context.view, context.Flag(doc::Prop::Open));
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                // Only the header is the switch. A press inside the body is the body's, and
                // consuming it here would make every link in an open section unclickable.
                if (IsLeftPress(event)) return OverHeader(context, PointOf(event));
                if (IsLeftRelease(event)) {
                    if (!OverHeader(context, PointOf(event))) return false;
                    Toggle(context);
                    return true;
                }
                if (event.type == EventType::KeyPressed
                    && (event.key.code == Key::Space || event.key.code == Key::Enter)) {
                    Toggle(context);
                    return true;
                }
                return false;
            }

        private:
            // The row that toggles: a child called "Header" if there is one, otherwise the first
            // child that is not the body — so a collapsible someone built by hand still works.
            static u32 HeaderOf(const ViewTree& tree, u32 view) {
                u32 fallback = ViewTree::kInvalid;
                for (u32 child : tree.At(view).children) {
                    if (tree.At(child).name == "Header") return child;
                    if (tree.At(child).role == Role::Content) continue;
                    if (fallback == ViewTree::kInvalid) fallback = child;
                }
                return fallback;
            }

            static bool OverHeader(const WidgetContext& context, Vec2 point) {
                const u32 header = HeaderOf(context.tree, context.view);
                if (header == ViewTree::kInvalid) return context.Bounds().Contains(point);
                return context.tree.Bounds(header).Contains(point);
            }

            static void Apply(ViewTree& tree, u32 view, bool open) {
                tree.SetState(view, StateBit::Open, open);
                const u32 content = tree.FindRole(view, Role::Content);
                if (content != ViewTree::kInvalid) tree.SetRuntimeVisible(content, open);
            }

            void Toggle(WidgetContext& context) {
                const bool next = !context.Flag(doc::Prop::Open);
                context.Set(doc::Prop::Open, next);
                Apply(context.tree, context.view, next);
                if (next) CloseSiblings(context);
                Fire(context, next ? ActionKind::Opened : ActionKind::Closed, doc::Value{ next });
            }

            // An Accordion is one-at-a-time by definition. A designer who wants several sections
            // open at once leaves the wrapper out and uses bare collapsibles, which is the same
            // distinction the web draws between `type="single"` and `type="multiple"`.
            static void CloseSiblings(WidgetContext& context) {
                ViewTree& tree = context.tree;
                const u32 accordion = AncestorWithRole(tree, context.view, Role::Accordion);
                if (accordion == ViewTree::kInvalid) return;

                for (u32 peer : tree.FindAllRoles(accordion, Role::Collapsible)) {
                    if (peer == context.view) continue;
                    if (!tree.Flag(peer, doc::Prop::Open)) continue;
                    tree.SetViewProp(peer, doc::Prop::Open, doc::Value{ false });
                    Apply(tree, peer, false);
                }
            }
        };

    }

    Scope<Behavior> MakeCollapsible() { return CreateScope<CollapsibleBehavior>(); }

}
