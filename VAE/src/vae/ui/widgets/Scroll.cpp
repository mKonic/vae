#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include "vae/text/TextDraw.h"

#include <algorithm>
#include <cmath>

namespace vae::ui::widgets {

    namespace {

        constexpr f32 kWheelStep = 48.0f;

        Vec2 MaxScroll(const ViewTree& tree, u32 view) {
            const Vec2 viewport = tree.Bounds(view).size;
            const Vec2 content = tree.ContentSize(view);
            return { std::max(content.x - viewport.x, 0.0f), std::max(content.y - viewport.y, 0.0f) };
        }

        bool ScrollBy(WidgetContext& context, u32 view, Vec2 delta) {
            const Vec2 limit = MaxScroll(context.tree, view);
            const Vec2 current = context.tree.At(view).scroll;
            const Vec2 next{ std::clamp(current.x + delta.x, 0.0f, limit.x),
                             std::clamp(current.y + delta.y, 0.0f, limit.y) };
            if (std::abs(next.x - current.x) < 0.01f && std::abs(next.y - current.y) < 0.01f)
                return false;
            context.tree.SetScroll(view, next);
            return true;
        }

        // Sizes and positions a scrollbar's thumb from the scroll state. The track and the thumb
        // are nodes the designer drew; this only moves them.
        void ApplyThumb(ViewTree& tree, u32 scroller, f32 contentHeight) {
            const u32 thumb = tree.FindRole(scroller, Role::Thumb);
            if (thumb == ViewTree::kInvalid) return;
            const u32 track = tree.At(thumb).parent;
            if (track == ViewTree::kInvalid) return;

            const f32 viewport = tree.Bounds(scroller).size.y;
            const f32 content = std::max(contentHeight, viewport);
            const f32 trackLength = tree.Bounds(track).size.y;
            const f32 fraction = content > 0.0f ? std::clamp(viewport / content, 0.05f, 1.0f) : 1.0f;

            layout::LayoutStyle style = tree.LayoutStyleOf(thumb);
            style.height = layout::Size::Px(std::max(trackLength * fraction, 16.0f));
            const f32 travel = std::max(trackLength - style.height.value, 0.0f);
            const f32 limit = std::max(content - viewport, 0.0f);
            const f32 t = limit > 0.0f ? tree.At(scroller).scroll.y / limit : 0.0f;
            style.offsetStart.y = t * travel;
            style.constraintY = layout::Constraint::Start;
            tree.SetLayoutStyle(thumb, style);
        }

        class ScrollBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Scroll; }
            bool Focusable() const override { return false; }

            void Arrange(WidgetContext& context) override {
                ApplyThumb(context.tree, context.view, context.tree.ContentSize(context.view).y);
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (event.type != EventType::MouseScrolled) return false;
                // Shift turns a vertical wheel into a horizontal one, which is the convention every
                // toolkit follows and the only way to scroll sideways with a plain mouse.
                const bool horizontal = (event.mods & Mod::Shift) != 0;
                Vec2 delta{ -event.scroll.dx * kWheelStep, -event.scroll.dy * kWheelStep };
                if (horizontal) delta = { delta.y, 0.0f };

                if (!ScrollBy(context, context.view, delta)) return false;
                Fire(context, ActionKind::Scrolled,
                     doc::Value{ context.tree.At(context.view).scroll.y });
                return true;
            }
        };

        class ThumbBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Thumb; }
            bool Focusable() const override { return false; }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                const u32 scroller = Scroller(context);
                if (scroller == ViewTree::kInvalid) return false;

                if (IsLeftPress(event)) {
                    m_Grab = PointOf(event).y - context.Bounds().Top();
                    context.SetState(StateBit::Pressed, true);
                    context.host.Capture(context.view);
                    return true;
                }
                if (event.type == EventType::MouseMoved && context.host.Captured() == context.view) {
                    const u32 track = context.Self().parent;
                    const f32 trackTop = context.tree.Bounds(track).Top();
                    const f32 travel = std::max(context.tree.Bounds(track).size.y
                                                - context.Bounds().size.y, 1.0f);
                    const f32 t = std::clamp((PointOf(event).y - m_Grab - trackTop) / travel, 0.0f, 1.0f);
                    const f32 limit = MaxScroll(context.tree, scroller).y;

                    WidgetContext target{ context.tree, context.host, scroller };
                    context.tree.SetScroll(scroller, { context.tree.At(scroller).scroll.x, 0.0f });
                    ScrollBy(target, scroller, { 0.0f, t * limit });
                    return true;
                }
                if (IsLeftRelease(event)) {
                    context.SetState(StateBit::Pressed, false);
                    return true;
                }
                return false;
            }

            void OnCaptureLost(WidgetContext& context) override {
                context.SetState(StateBit::Pressed, false);
            }

        private:
            static u32 Scroller(const WidgetContext& context) {
                for (u32 i = context.view; i != ViewTree::kInvalid; i = context.tree.At(i).parent) {
                    const Role role = context.tree.At(i).role;
                    if (role == Role::Scroll || role == Role::List || role == Role::Table) return i;
                }
                return ViewTree::kInvalid;
            }

            f32 m_Grab = 0.0f;
        };

        // A virtualized list draws rows; it does not own a node per row. The template node is a
        // real node the designer styles, hidden at runtime and stamped once per visible row — which
        // is what lets a million-row table cost one template and a scroll offset.
        class ListBehavior : public Behavior {
        public:
            explicit ListBehavior(bool table) : m_Table(table) {}

            Role Kind() const override { return m_Table ? Role::Table : Role::List; }

            void Sync(WidgetContext& context) override {
                const u32 templateRow = Template(context);
                if (templateRow != ViewTree::kInvalid)
                    context.tree.SetRuntimeVisible(templateRow, false);
            }

            void Arrange(WidgetContext& context) override {
                ApplyThumb(context.tree, context.view,
                           static_cast<f32>(RowCount(context)) * RowHeight(context));
            }

            bool OnEvent(WidgetContext& context, const Event& event) override {
                if (SwallowedWhileDisabled(context, event)) return true;

                if (event.type == EventType::MouseScrolled) {
                    const Vec2 delta{ 0.0f, -event.scroll.dy * kWheelStep };
                    return ScrollVirtual(context, delta);
                }
                if (IsLeftPress(event)) {
                    const i32 row = RowAt(context, PointOf(event).y);
                    if (row >= 0) Select(context, row);
                    return true;
                }
                if (event.type == EventType::KeyPressed) {
                    const i32 count = static_cast<i32>(RowCount(context));
                    if (count == 0) return false;
                    const i32 current = static_cast<i32>(context.Number(doc::Prop::SelectedIndex, -1.0f));
                    const i32 page = std::max(static_cast<i32>(context.Bounds().size.y
                                                               / RowHeight(context)), 1);
                    switch (event.key.code) {
                        case Key::Up:       Select(context, std::max(current - 1, 0)); return true;
                        case Key::Down:     Select(context, std::min(current + 1, count - 1)); return true;
                        case Key::PageUp:   Select(context, std::max(current - page, 0)); return true;
                        case Key::PageDown: Select(context, std::min(current + page, count - 1)); return true;
                        case Key::Home:     Select(context, 0); return true;
                        case Key::End:      Select(context, count - 1); return true;
                        default: return false;
                    }
                }
                return false;
            }

            void OnPaint(const WidgetContext& context, PaintContext& paint) const override {
                if (!paint.list) return;
                const u32 templateRow = Template(context);
                if (templateRow == ViewTree::kInvalid) return;

                const u32 count = RowCount(context);
                if (count == 0) return;

                const Rect box = context.Bounds();
                const f32 rowHeight = RowHeight(context);
                const f32 offset = context.tree.At(context.view).scroll.y;
                const f32 headerHeight = HeaderHeight(context);
                const Rect body{ { box.pos.x, box.pos.y + headerHeight },
                                 { box.size.x, std::max(box.size.y - headerHeight, 0.0f) } };

                // Only the rows on screen are touched, plus one either side so a partially visible
                // row at the edge is drawn rather than popping in.
                const auto first = static_cast<u32>(std::max(std::floor(offset / rowHeight) - 1.0f, 0.0f));
                const auto visible = static_cast<u32>(std::ceil(body.size.y / rowHeight)) + 2;
                const u32 last = std::min(first + visible, count);

                const doc::PropBag style = context.tree.Resolved(templateRow);
                const Color fill = style.Colour(doc::Prop::Fill, { 0, 0, 0, 0 });
                // The selected and hovered row colours come from the template's own state overlays,
                // so a designer restyles a virtual row exactly as they would a real one.
                const Color selectedFill = OverlayColour(context, templateRow, StateBit::Selected, fill);
                const Color hoveredFill = OverlayColour(context, templateRow, StateBit::Hovered, fill);
                const i32 hoveredRow = context.host.Hovered() == context.view
                                     ? RowAt(context, context.host.MousePosition().y) : -1;
                const Color textColour = style.Colour(doc::Prop::TextColor, { 1, 1, 1, 1 });
                const f32 radius = style.Number(doc::Prop::CornerRadius, 0.0f);
                const auto selected = static_cast<i32>(context.Number(doc::Prop::SelectedIndex, -1.0f));
                const auto* source = context.host.DataSource(context.Id());
                const auto columns = m_Table ? context.tree.FindAllRoles(context.view, Role::TableColumn)
                                             : std::vector<u32>{};

                paint.list->PushClip(body);
                for (u32 row = first; row < last; ++row) {
                    const Rect rect{ { body.pos.x, body.pos.y + static_cast<f32>(row) * rowHeight - offset },
                                     { body.size.x, rowHeight } };
                    if (rect.Bottom() < body.Top() || rect.Top() > body.Bottom()) continue;

                    Color background = fill;
                    if (static_cast<i32>(row) == hoveredRow) background = hoveredFill;
                    if (static_cast<i32>(row) == selected) background = selectedFill;
                    if (background.a > 0.0f)
                        paint.list->AddRect(rect, draw::Paint::Solid(background), Corners{ radius });

                    if (!paint.atlas) continue;
                    const text::TextStyle textStyle = context.tree.StyleFor(templateRow);
                    if (!textStyle.font) continue;
                    // No data source is the designer's normal case: the rows come from the app at
                    // run time. Drawing the bands and nothing else makes a table look broken while
                    // it is being laid out, so the cells stand in until something fills them.
                    const auto cell = [&](u32 index) {
                        return source ? source->Cell(row, index) : std::string("—");
                    };
                    const Color ink = source ? textColour
                                             : Color{ textColour.r, textColour.g, textColour.b,
                                                      textColour.a * 0.45f };
                    const f32 baseline = rect.pos.y
                        + std::max((rowHeight - textStyle.font->Metrics(textStyle.size).LineHeight())
                                   * 0.5f, 0.0f);

                    if (columns.empty()) {
                        text::DrawText(*paint.list, *paint.atlas, cell(0), textStyle,
                                       { rect.pos.x + 8.0f, baseline }, ink,
                                       rect.size.x - 16.0f, text::WrapMode::None);
                        continue;
                    }
                    f32 x = rect.pos.x;
                    for (u32 column = 0; column < columns.size(); ++column) {
                        const f32 width = context.tree.Bounds(columns[column]).size.x;
                        text::DrawText(*paint.list, *paint.atlas, cell(column), textStyle,
                                       { x + 8.0f, baseline }, ink,
                                       std::max(width - 16.0f, 0.0f), text::WrapMode::None);
                        x += width;
                    }
                }
                paint.list->PopClip();
            }

        protected:
            static Color OverlayColour(const WidgetContext& context, u32 view, StateBit bit,
                                       Color fallback) {
                const doc::Value* value =
                    context.tree.At(view).props.Find(StateKey(bit, doc::Prop::Fill));
                if (!value) return fallback;
                const doc::Value resolved = context.tree.Document().ResolveValue(*value);
                const Color* colour = std::get_if<Color>(&resolved);
                return colour ? *colour : fallback;
            }

            static u32 Template(const WidgetContext& context) {
                return context.tree.FindRole(context.view, Role::ListItem);
            }

            static f32 RowHeight(const WidgetContext& context) {
                const f32 authored = context.Number(doc::Prop::ItemHeight, 0.0f);
                if (authored > 0.0f) return authored;
                const u32 templateRow = Template(context);
                const f32 measured = templateRow == ViewTree::kInvalid
                                   ? 0.0f : context.tree.Bounds(templateRow).size.y;
                return measured > 1.0f ? measured : 28.0f;
            }

            f32 HeaderHeight(const WidgetContext& context) const {
                if (!m_Table) return 0.0f;
                const auto columns = context.tree.FindAllRoles(context.view, Role::TableColumn);
                if (columns.empty()) return 0.0f;
                const u32 header = context.tree.At(columns.front()).parent;
                return header == ViewTree::kInvalid ? 0.0f : context.tree.Bounds(header).size.y;
            }

            static u32 RowCount(const WidgetContext& context) {
                if (const auto* source = context.host.DataSource(context.Id()))
                    return source->Count();
                return static_cast<u32>(std::max(context.Number(doc::Prop::ItemCount, 0.0f), 0.0f));
            }

            // The scrollable extent of a virtual list is arithmetic, not a measured subtree — the
            // rows it would need to measure do not exist.
            bool ScrollVirtual(WidgetContext& context, Vec2 delta) const {
                const f32 content = static_cast<f32>(RowCount(context)) * RowHeight(context);
                const f32 viewport = std::max(context.Bounds().size.y - HeaderHeight(context), 0.0f);
                const f32 limit = std::max(content - viewport, 0.0f);
                const Vec2 current = context.tree.At(context.view).scroll;
                const f32 next = std::clamp(current.y + delta.y, 0.0f, limit);
                if (std::abs(next - current.y) < 0.01f) return false;
                context.tree.SetScroll(context.view, { current.x, next });
                return true;
            }

            i32 RowAt(const WidgetContext& context, f32 y) const {
                const Rect box = context.Bounds();
                const f32 header = HeaderHeight(context);
                const f32 local = y - box.pos.y - header + context.tree.At(context.view).scroll.y;
                if (local < 0.0f) return -1;
                const auto row = static_cast<i32>(local / RowHeight(context));
                return row >= 0 && row < static_cast<i32>(RowCount(context)) ? row : -1;
            }

            void Select(WidgetContext& context, i32 row) const {
                if (static_cast<i32>(context.Number(doc::Prop::SelectedIndex, -1.0f)) == row) return;
                context.Set(doc::Prop::SelectedIndex, static_cast<f32>(row));
                ScrollIntoView(context, row);
                Fire(context, ActionKind::SelectionChanged, doc::Value{ static_cast<f32>(row) });
            }

            void ScrollIntoView(const WidgetContext& context, i32 row) const {
                const f32 rowHeight = RowHeight(context);
                const f32 viewport = std::max(context.Bounds().size.y - HeaderHeight(context), 0.0f);
                const f32 top = static_cast<f32>(row) * rowHeight;
                const f32 current = context.tree.At(context.view).scroll.y;
                f32 next = current;
                if (top < current) next = top;
                else if (top + rowHeight > current + viewport) next = top + rowHeight - viewport;
                if (next != current)
                    context.tree.SetScroll(context.view, { context.tree.At(context.view).scroll.x, next });
            }

            bool m_Table;
        };

    }

    Scope<Behavior> MakeScroll() { return CreateScope<ScrollBehavior>(); }
    Scope<Behavior> MakeThumb()  { return CreateScope<ThumbBehavior>(); }
    Scope<Behavior> MakeList()   { return CreateScope<ListBehavior>(false); }
    Scope<Behavior> MakeTable()  { return CreateScope<ListBehavior>(true); }

}
