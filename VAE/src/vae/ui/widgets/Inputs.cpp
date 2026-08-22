#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include "vae/base/Utf8.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace vae::ui::widgets {

    namespace {

        u32 NamedChild(const ViewTree& tree, u32 view, std::string_view name) {
            for (u32 child : tree.At(view).children)
                if (tree.At(child).name == name) return child;
            return ViewTree::kInvalid;
        }

        // The first Text node under a view, ignoring nothing: these parts have exactly one.
        u32 TextIn(const ViewTree& tree, u32 view) {
            std::vector<u32> stack{ view };
            while (!stack.empty()) {
                const u32 current = stack.back();
                stack.pop_back();
                if (tree.At(current).kind == doc::NodeKind::Text) return current;
                for (u32 child : tree.At(current).children) stack.push_back(child);
            }
            return ViewTree::kInvalid;
        }

        // ------------------------------------------------------------------------- one-time code

        // A row of boxes holding one character each. It is a text field whose glyphs happen to be
        // apart, so the value is Prop::Text like every other field and a script reads it the same
        // way — the boxes are a picture of the string, not six strings.
        class InputOtpBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::InputOtp; }
            bool Focusable() const override { return true; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                return context.Enabled() ? CursorShape::IBeam : CursorShape::NotAllowed;
            }

            void Sync(WidgetContext& context) override { Show(context); }
            // Also every frame, not only on a rebuild: which box is next depends on focus, and
            // gaining focus changes no document property for Sync to notice.
            void Arrange(WidgetContext& context) override { Show(context); }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                if (IsLeftPress(event)) {
                    context.host.Focus(context.view);
                    return true;
                }
                if (event.type == EventType::TextInput) {
                    // Digits and letters only: a one-time code has no punctuation in it, and
                    // filtering here is what stops a paste of "1 2 3" filling three boxes with two
                    // spaces.
                    const u32 code = event.text.codepoint;
                    if (code > 0x7F || std::isalnum(static_cast<unsigned char>(code)) == 0) return true;
                    std::string value = context.Str(doc::Prop::Text);
                    if (value.size() >= Boxes(context)) return true;
                    value += static_cast<char>(std::toupper(static_cast<unsigned char>(code)));
                    Commit(context, std::move(value));
                    return true;
                }
                if (event.type == EventType::KeyPressed) {
                    std::string value = context.Str(doc::Prop::Text);
                    if (event.key.code == Key::Backspace) {
                        if (!value.empty()) value.pop_back();
                        Commit(context, std::move(value));
                        return true;
                    }
                    if (event.key.code == Key::Enter && value.size() == Boxes(context)) {
                        Fire(context, ActionKind::Submitted, doc::Value{ value });
                        return true;
                    }
                }
                return false;
            }

        private:
            static std::size_t Boxes(const WidgetContext& context) {
                return context.tree.At(context.view).children.size();
            }

            void Commit(WidgetContext& context, std::string value) {
                const bool full = value.size() == Boxes(context);
                context.Set(doc::Prop::Text, value);
                Show(context);
                Fire(context, ActionKind::TextChanged, doc::Value{ value });
                // Filling the last box is the whole gesture: nobody presses Enter after a code.
                if (full) Fire(context, ActionKind::Submitted, doc::Value{ std::move(value) });
            }

            static void Show(WidgetContext& context) {
                ViewTree& tree = context.tree;
                const std::string value = context.Str(doc::Prop::Text);
                const auto& boxes = tree.At(context.view).children;
                const bool focused = context.host.Focused() == context.view;

                for (std::size_t i = 0; i < boxes.size(); ++i) {
                    const u32 label = TextIn(tree, boxes[i]);
                    if (label != ViewTree::kInvalid)
                        tree.At(label).props.Set(doc::Prop::Text,
                                                 doc::Value{ i < value.size()
                                                             ? std::string(1, value[i])
                                                             : std::string{} });
                    // The caret is a box, not a bar: the next box to be filled is the lit one.
                    // Selected, not Focused — a widget's own Focused reaches every part of it, so
                    // styling on that would light all six the moment the row is focused at all.
                    tree.SetState(boxes[i], StateBit::Selected, focused && i == value.size());
                }
            }
        };

        // ------------------------------------------------------------------------- carousel

        // A strip of slides with one showing. The offset is applied to the track rather than the
        // scroll, so the movement can be animated later without the scrollbar machinery deciding
        // it is a scroll.
        class CarouselBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Carousel; }
            bool Focusable() const override { return true; }

            CursorShape CursorOver(const WidgetContext& context) const override {
                if (!context.Enabled()) return CursorShape::NotAllowed;
                return Step(context, context.host.MousePosition()) != 0 ? CursorShape::Hand
                                                                        : CursorShape::Arrow;
            }

            void Sync(WidgetContext& context) override { Mark(context); }
            void Arrange(WidgetContext& context) override { Place(context); }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;
                if (!context.Enabled()) return false;

                if (IsLeftPress(event)) return Step(context, PointOf(event)) != 0;
                if (IsLeftRelease(event)) {
                    const i32 step = Step(context, PointOf(event));
                    if (step == 0) return false;
                    GoTo(context, Index(context) + step);
                    return true;
                }
                if (event.type == EventType::KeyPressed) {
                    if (event.key.code == Key::Left)  { GoTo(context, Index(context) - 1); return true; }
                    if (event.key.code == Key::Right) { GoTo(context, Index(context) + 1); return true; }
                }
                return false;
            }

        private:
            static u32 Track(const ViewTree& tree, u32 view) {
                return tree.FindRole(view, Role::Content);
            }
            static i32 Count(const WidgetContext& context) {
                const u32 track = Track(context.tree, context.view);
                return track == ViewTree::kInvalid
                     ? 0 : static_cast<i32>(context.tree.At(track).children.size());
            }
            static i32 Index(const WidgetContext& context) {
                return static_cast<i32>(std::lround(context.Number(doc::Prop::SelectedIndex, 0.0f)));
            }

            static i32 Step(const WidgetContext& context, Vec2 at) {
                const ViewTree& tree = context.tree;
                const u32 prev = NamedChild(tree, context.view, "Prev");
                const u32 next = NamedChild(tree, context.view, "Next");
                if (prev != ViewTree::kInvalid && tree.Bounds(prev).Contains(at)) return -1;
                if (next != ViewTree::kInvalid && tree.Bounds(next).Contains(at)) return 1;
                return 0;
            }

            static void Mark(WidgetContext& context) {
                ViewTree& tree = context.tree;
                const i32 index = Index(context);
                const i32 count = Count(context);
                const u32 prev = NamedChild(tree, context.view, "Prev");
                const u32 next = NamedChild(tree, context.view, "Next");
                if (prev != ViewTree::kInvalid) tree.SetState(prev, StateBit::Disabled, index <= 0);
                if (next != ViewTree::kInvalid)
                    tree.SetState(next, StateBit::Disabled, index >= count - 1);

                // The dots, if the designer drew any: one Indicator per slide.
                const auto dots = tree.FindAllRoles(context.view, Role::Indicator);
                for (u32 i = 0; i < dots.size(); ++i)
                    tree.SetState(dots[i], StateBit::Selected, static_cast<i32>(i) == index);
            }

            static void Place(WidgetContext& context) {
                ViewTree& tree = context.tree;
                const u32 track = Track(tree, context.view);
                if (track == ViewTree::kInvalid) return;
                const auto& slides = tree.At(track).children;
                if (slides.empty()) return;

                const i32 index = std::clamp(Index(context), 0, static_cast<i32>(slides.size()) - 1);
                const f32 slide = tree.Bounds(slides[0]).size.x
                                + tree.LayoutStyleOf(track).gap;
                layout::LayoutStyle style = tree.LayoutStyleOf(track);
                style.constraintX = layout::Constraint::Start;
                style.offsetStart.x = -static_cast<f32>(index) * slide;
                tree.SetLayoutStyle(track, style);
            }

            static void GoTo(WidgetContext& context, i32 index) {
                const i32 count = Count(context);
                if (count == 0) return;
                index = std::clamp(index, 0, count - 1);
                if (index == Index(context)) return;
                context.Set(doc::Prop::SelectedIndex, static_cast<f32>(index));
                Mark(context);
                Place(context);
                Fire(context, ActionKind::SelectionChanged, doc::Value{ static_cast<f32>(index) });
            }
        };

    }

    Scope<Behavior> MakeInputOtp() { return CreateScope<InputOtpBehavior>(); }
    Scope<Behavior> MakeCarousel() { return CreateScope<CarouselBehavior>(); }

}
