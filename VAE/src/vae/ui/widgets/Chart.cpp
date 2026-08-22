#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace vae::ui::widgets {

    namespace {

        // A plot of numbers held on the document. The numbers are a property, not a data source,
        // because the first thing anyone does with a chart is lay one out — and a chart that is
        // blank until an app runs is a chart nobody can design against. A script writes the same
        // property when there is real data, so the two are the same chart.
        class ChartBehavior final : public Behavior {
        public:
            Role Kind() const override { return Role::Chart; }
            bool Focusable() const override { return false; }

            void OnPaint(const WidgetContext& context, PaintContext& paint) const override {
                if (!paint.list) return;
                const std::vector<f32> values = Series(context);
                const Rect box = Plot(context);
                if (values.empty() || box.size.x <= 1.0f || box.size.y <= 1.0f) return;

                f32 low = context.Number(doc::Prop::MinValue, 0.0f);
                f32 high = context.Number(doc::Prop::MaxValue, 0.0f);
                if (high <= low) {
                    // Auto-scale, from zero rather than from the smallest value: a bar chart that
                    // does not start at zero is a bar chart that lies about its proportions.
                    low = std::min(0.0f, *std::min_element(values.begin(), values.end()));
                    high = *std::max_element(values.begin(), values.end());
                    if (high <= low) high = low + 1.0f;
                }
                const f32 span = high - low;

                // The plot's colours come from hidden part nodes, not from the frame's own stroke:
                // a chart is a surface like any other and its border is its border. Reading the
                // line colour off Prop::Stroke would make every chart wear its plot as an outline.
                const Color line = PartColour(context, Role::Indicator, { 0.4f, 0.6f, 0.95f, 1.0f });
                const Color area = PartColour(context, Role::Fill,
                                              { line.r, line.g, line.b, line.a * 0.25f });
                const Color grid = PartColour(context, Role::Track, { 1.0f, 1.0f, 1.0f, 0.06f });
                const f32 width = std::max(PartNumber(context, Role::Indicator,
                                                      doc::Prop::StrokeWidth, 2.0f), 1.0f);

                paint.list->PushClip(box);
                for (int i = 0; i <= 4; ++i) {
                    const f32 y = box.Top() + box.size.y * static_cast<f32>(i) / 4.0f;
                    paint.list->AddRect(Rect{ { box.Left(), y }, { box.size.x, 1.0f } },
                                        draw::Paint::Solid(grid));
                }

                const auto at = [&](std::size_t index) {
                    const f32 t = values.size() < 2 ? 0.5f
                                : static_cast<f32>(index) / static_cast<f32>(values.size() - 1);
                    const f32 height = (values[index] - low) / span;
                    return Vec2{ box.Left() + t * box.size.x,
                                 box.Bottom() - std::clamp(height, 0.0f, 1.0f) * box.size.y };
                };

                const std::string kind = context.Str(doc::Prop::ChartKind, "line");
                if (kind == "bars") {
                    const f32 slot = box.size.x / static_cast<f32>(values.size());
                    const f32 bar = std::max(slot * 0.62f, 1.0f);
                    for (std::size_t i = 0; i < values.size(); ++i) {
                        const f32 top = at(i).y;
                        const f32 x = box.Left() + slot * (static_cast<f32>(i) + 0.5f) - bar * 0.5f;
                        paint.list->AddRect(Rect{ { x, top }, { bar, box.Bottom() - top } },
                                            draw::Paint::Solid(line), Corners{ 3.0f });
                    }
                    paint.list->PopClip();
                    return;
                }

                if (kind == "area") {
                    // A column per two pixels: the renderer draws boxes and lines, and a filled
                    // polygon under a curve is neither. Close enough that nobody can tell, and it
                    // costs a few hundred quads on a chart the width of a page.
                    const f32 step = 2.0f;
                    for (f32 x = box.Left(); x < box.Right(); x += step) {
                        const f32 t = (x - box.Left()) / box.size.x
                                    * static_cast<f32>(values.size() - 1);
                        const auto index = static_cast<std::size_t>(t);
                        const f32 frac = t - static_cast<f32>(index);
                        const Vec2 a = at(std::min(index, values.size() - 1));
                        const Vec2 b = at(std::min(index + 1, values.size() - 1));
                        const f32 y = a.y + (b.y - a.y) * frac;
                        paint.list->AddRect(Rect{ { x, y },
                                                  { std::min(step, box.Right() - x),
                                                    box.Bottom() - y } },
                                            draw::Paint::Solid(area));
                    }
                }

                for (std::size_t i = 1; i < values.size(); ++i)
                    paint.list->AddLine(at(i - 1), at(i), width, line);
                paint.list->PopClip();
            }

        private:
            // The area the plot occupies, which is the widget minus whatever padding the designer
            // gave it — so a chart with a caption in it does not draw over the caption.
            static Rect Plot(const WidgetContext& context) {
                const Rect box = context.Bounds();
                const Edges pad = context.tree.LayoutStyleOf(context.view).padding;
                return Rect{ { box.pos.x + pad.left, box.pos.y + pad.top },
                             { std::max(box.size.x - pad.Horizontal(), 0.0f),
                               std::max(box.size.y - pad.Vertical(), 0.0f) } };
            }

            // A hidden part node the designer styles: it takes no space and draws nothing itself,
            // it just says what colour that piece of the plot is. Opacity is honoured here because
            // nothing else will — the node is never painted.
            static f32 PartNumber(const WidgetContext& context, Role role, doc::Prop prop,
                                  f32 fallback) {
                const u32 part = context.tree.FindRole(context.view, role);
                return part == ViewTree::kInvalid ? fallback
                                                  : context.tree.Number(part, prop, fallback);
            }

            static Color PartColour(const WidgetContext& context, Role role, Color fallback) {
                const u32 part = context.tree.FindRole(context.view, role);
                if (part == ViewTree::kInvalid) return fallback;
                const doc::PropBag props = context.tree.Resolved(part);
                Color colour = props.Colour(doc::Prop::Fill, fallback);
                colour.a *= std::clamp(props.Number(doc::Prop::FillOpacity, 1.0f), 0.0f, 1.0f);
                return colour;
            }

            // "12, 19, 3 14" — commas or spaces, because a designer typing a row of numbers should
            // not have to think about which.
            static std::vector<f32> Series(const WidgetContext& context) {
                const std::string text = context.Str(doc::Prop::Series);
                std::vector<f32> values;
                std::size_t at = 0;
                while (at < text.size()) {
                    while (at < text.size() && (text[at] == ',' || std::isspace(
                               static_cast<unsigned char>(text[at])))) ++at;
                    if (at >= text.size()) break;
                    const char* begin = text.data() + at;
                    const char* end = text.data() + text.size();
                    f32 value = 0.0f;
                    const auto [stop, error] = std::from_chars(begin, end, value);
                    if (error != std::errc{}) break;
                    values.push_back(value);
                    at = static_cast<std::size_t>(stop - text.data());
                }
                return values;
            }
        };

    }

    Scope<Behavior> MakeChart() { return CreateScope<ChartBehavior>(); }

}
