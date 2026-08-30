#include "vaepch.h"
#include "vae/layout/LayoutTree.h"

#include <algorithm>
#include <cmath>

namespace vae::layout {

    namespace {
        f32 Main(Vec2 v, Axis axis)  { return axis == Axis::Row ? v.x : v.y; }
        f32 Cross(Vec2 v, Axis axis) { return axis == Axis::Row ? v.y : v.x; }
        Vec2 Compose(f32 main, f32 cross, Axis axis) {
            return axis == Axis::Row ? Vec2{ main, cross } : Vec2{ cross, main };
        }
        // Main and cross are relative to the axis of the stack a node SITS IN, never to the node's
        // own `axis` field (which describes how it lays out its own children). Reading the child's
        // axis here made a row stack size its children by their heights.
        const Size& MainSize(const LayoutStyle& s, Axis parentAxis) {
            return parentAxis == Axis::Row ? s.width : s.height;
        }
        const Size& CrossSize(const LayoutStyle& s, Axis parentAxis) {
            return parentAxis == Axis::Row ? s.height : s.width;
        }
    }

    void LayoutTree::Clear() { m_Nodes.clear(); }

    u32 LayoutTree::Add(const LayoutStyle& style, u32 parent) {
        const u32 index = static_cast<u32>(m_Nodes.size());
        Node node;
        node.style = style;
        node.parent = parent;
        m_Nodes.push_back(std::move(node));
        if (parent != kInvalid) m_Nodes[parent].children.push_back(index);
        return index;
    }

    void LayoutTree::SetIntrinsic(u32 node, Vec2 size) { m_Nodes[node].intrinsic = size; }

    void LayoutTree::SetMeasure(u32 node, MeasureFn measure) {
        m_Nodes[node].measure = std::move(measure);
        m_Nodes[node].measuredAt = -1.0f;
        m_Nodes[node].forcedWidth = -1.0f;
    }
    void LayoutTree::SetExcluded(u32 node, bool excluded) { m_Nodes[node].excluded = excluded; }
    void LayoutTree::SetStyle(u32 node, const LayoutStyle& style) { m_Nodes[node].style = style; }

    Rect LayoutTree::AbsoluteRect(u32 node) const {
        Rect rect = m_Nodes[node].rect;
        for (u32 parent = m_Nodes[node].parent; parent != kInvalid; parent = m_Nodes[parent].parent)
            rect.pos += m_Nodes[parent].rect.pos;
        return rect;
    }

    Vec2 LayoutTree::Clamp(const Node& node, Vec2 size) const {
        return { std::clamp(size.x, node.style.minSize.x, node.style.maxSize.x),
                 std::clamp(size.y, node.style.minSize.y, node.style.maxSize.y) };
    }

    Vec2 LayoutTree::ApplyAspect(const Node& node, Vec2 size, bool widthKnown, bool heightKnown) const {
        const f32 ratio = node.style.aspectRatio;
        if (ratio <= 0.0f) return size;

        // Whichever axis is already pinned drives the other. If both are pinned the ratio loses —
        // an explicit size is a stronger statement than a ratio.
        if (widthKnown && !heightKnown) size.y = size.x / ratio;
        else if (heightKnown && !widthKnown) size.x = size.y * ratio;
        else if (!widthKnown && !heightKnown && size.x > 0.0f) size.y = size.x / ratio;
        return size;
    }

    Vec2 LayoutTree::AspectFit(const Node& node, Vec2 size) const {
        const f32 ratio = node.style.aspectRatio;
        if (ratio <= 0.0f) return size;

        // Whichever axis the author stated drives the other. Both stated, and the ratio loses: an
        // explicit size is a stronger statement than a proportion.
        const bool widthStated  = node.style.width.mode  != SizeMode::Hug;
        const bool heightStated = node.style.height.mode != SizeMode::Hug;
        if (widthStated && !heightStated) size.y = size.x / ratio;
        else if (heightStated && !widthStated) size.x = size.y * ratio;
        return size;
    }

    // ---------------------------------------------------------------- measure

    Vec2 LayoutTree::Measure(u32 index, Vec2 available) {
        Node& node = m_Nodes[index];
        const LayoutStyle& style = node.style;

        const bool widthFixed  = style.width.mode  == SizeMode::Fixed;
        const bool heightFixed = style.height.mode == SizeMode::Fixed;

        // Percent resolves against the parent's content box, which the caller passes down as
        // `available`. Fill contributes nothing here: a node that wants "whatever is left" cannot
        // also be what determines how much there is.
        const bool widthKnown  = widthFixed  || style.width.mode  == SizeMode::Percent;
        const bool heightKnown = heightFixed || style.height.mode == SizeMode::Percent;

        Vec2 own{ 0.0f, 0.0f };
        if (widthFixed)  own.x = style.width.value;
        else if (style.width.mode == SizeMode::Percent && std::isfinite(available.x))
            own.x = available.x * style.width.value;
        if (heightFixed) own.y = style.height.value;
        else if (style.height.mode == SizeMode::Percent && std::isfinite(available.y))
            own.y = available.y * style.height.value;

        // What the children need, measured inside whatever this node already knows about itself.
        Vec2 inner{
            widthKnown  ? std::max(own.x - style.padding.Horizontal(), 0.0f) : available.x,
            heightKnown ? std::max(own.y - style.padding.Vertical(),   0.0f) : available.y,
        };

        // A container that reflows answers a different height at every width, exactly as wrapped
        // text does. Measure is offered the room the parent *has*; arrange hands out the room that
        // is left. Recording the width this answer was given lets the second pass ask again at the
        // width that won — without it a grid beside anything else hangs its last rows outside a
        // parent that hugged the height of a wider layout.
        // An aspect ratio is the same problem from the other side: the height comes from the
        // width, so a Fill-width banner measures at a guess and arranges at the real thing. The
        // parent hugs the guess and draws the next sibling on top of it.
        const bool reflows = (!node.children.empty()
                              && (style.mode == LayoutMode::Grid
                                  || (style.mode == LayoutMode::Stack && style.wrap)))
                          || (style.aspectRatio > 0.0f && !widthKnown && !heightKnown);
        const bool reflowsByWidth = !node.children.empty()
                                  && (style.mode == LayoutMode::Grid
                                      || (style.mode == LayoutMode::Stack && style.wrap));
        if (reflows && node.forcedWidth >= 0.0f) inner.x = node.forcedWidth;

        Vec2 content{ 0.0f, 0.0f };

        if (node.children.empty()) {
            if (node.measure) {
                const Vec2 box = node.forcedWidth >= 0.0f ? Vec2{ node.forcedWidth, inner.y } : inner;
                content = node.measure(box);
                node.measuredAt = box.x;
            } else {
                content = node.intrinsic;
            }
        } else if (style.mode == LayoutMode::Stack) {
            const Axis axis = style.axis;
            f32 mainTotal = 0.0f, crossMax = 0.0f;
            u32 counted = 0;
            for (u32 child : node.children) {
                if (m_Nodes[child].excluded) continue;
                const Vec2 size = Measure(child, inner);
                const LayoutStyle& cs = m_Nodes[child].style;

                // A Fill child contributes nothing along the main axis (it consumes leftovers), but
                // still contributes its measured extent across it.
                if (MainSize(cs, axis).mode != SizeMode::Fill) mainTotal += Main(size, axis);
                if (CrossSize(cs, axis).mode != SizeMode::Fill)
                    crossMax = std::max(crossMax, Cross(size, axis));
                ++counted;
            }
            if (counted > 1) mainTotal += style.gap * static_cast<f32>(counted - 1);

            const f32 limit = Main(inner, axis);
            if (style.wrap && std::isfinite(limit) && mainTotal > limit) {
                // Re-measure as wrapped: the main extent is capped at the limit and the cross
                // extent is the sum of the lines. Without this a wrapping container hugs to the
                // height of one line and its own children overflow it.
                f32 lineMain = 0.0f, lineCross = 0.0f, crossTotal = 0.0f;
                u32 onLine = 0;
                for (u32 child : node.children) {
                    if (m_Nodes[child].excluded) continue;
                    const Vec2 size = m_Nodes[child].measured;
                    const f32 childMain = Main(size, axis);
                    const f32 withGap = onLine > 0 ? style.gap : 0.0f;
                    if (onLine > 0 && lineMain + withGap + childMain > limit) {
                        crossTotal += lineCross + style.gap;
                        lineMain = 0.0f; lineCross = 0.0f; onLine = 0;
                    }
                    lineMain += (onLine > 0 ? style.gap : 0.0f) + childMain;
                    lineCross = std::max(lineCross, Cross(size, axis));
                    ++onLine;
                }
                crossTotal += lineCross;
                content = Compose(std::min(mainTotal, limit), crossTotal, axis);
            } else {
                content = Compose(mainTotal, crossMax, axis);
            }
        } else if (style.mode == LayoutMode::Grid) {
            // Every cell is one track wide, so a child's height is measured against that width and
            // not against the whole row — measuring wrapped text against the row is how a grid ends
            // up one line tall and overflowing.
            u32 columns = 1;
            f32 track = 0.0f;
            GridTracks(style, inner.x, columns, track);

            const f32 rowGap = style.rowGap > 0.0f ? style.rowGap : style.gap;
            f32 rowHeight = 0.0f, total = 0.0f;
            u32 inRow = 0, rows = 0;
            for (u32 child : node.children) {
                if (m_Nodes[child].excluded) continue;
                const Vec2 size = Measure(child, { track, inner.y });
                rowHeight = std::max(rowHeight, size.y);
                if (++inRow == columns) {
                    total += rowHeight + (rows > 0 ? rowGap : 0.0f);
                    ++rows;
                    rowHeight = 0.0f;
                    inRow = 0;
                }
            }
            if (inRow > 0) { total += rowHeight + (rows > 0 ? rowGap : 0.0f); ++rows; }

            const f32 width = std::isfinite(inner.x)
                            ? inner.x
                            : track * columns + style.gap * static_cast<f32>(columns - 1);
            content = { width, total };
        } else {
            // Absolute: the content box has to reach the far edge of every child.
            for (u32 child : node.children) {
                if (m_Nodes[child].excluded) continue;
                const Vec2 size = Measure(child, inner);
                const LayoutStyle& cs = m_Nodes[child].style;
                const f32 right  = (cs.constraintX == Constraint::End) ? size.x
                                                                      : cs.offsetStart.x + size.x;
                const f32 bottom = (cs.constraintY == Constraint::End) ? size.y
                                                                      : cs.offsetStart.y + size.y;
                content.x = std::max(content.x, right);
                content.y = std::max(content.y, bottom);
            }
        }

        Vec2 measured = own;
        if (!widthKnown)  measured.x = content.x + style.padding.Horizontal();
        if (!heightKnown) measured.y = content.y + style.padding.Vertical();
        // Second pass: this node's width is settled, so answer at that rather than at what its
        // content happens to need — which for an aspect ratio is the whole answer.
        if (reflows && node.forcedWidth >= 0.0f && !widthKnown)
            measured.x = node.forcedWidth + style.padding.Horizontal();

        // The width this answer was actually based on, so the second pass can tell whether the
        // width the node ended up with was the one it was asked about. For a grid that is the room
        // it laid its tracks out in; for an aspect ratio it is the width the answer came from,
        // which is its content's — not the room its parent happened to offer.
        if (reflows) {
            const f32 usedWidth = reflowsByWidth ? inner.x
                                                 : measured.x - style.padding.Horizontal();
            node.measuredAt = std::isfinite(usedWidth) ? usedWidth : -1.0f;
        }

        measured = ApplyAspect(node, measured, widthKnown, heightKnown);
        measured = Clamp(node, measured);

        node.measured = measured;
        return measured;
    }

    // ---------------------------------------------------------------- arrange

    void LayoutTree::ArrangeStack(Node& node, Rect content) {
        const LayoutStyle& style = node.style;
        const Axis axis = style.axis;
        const f32 mainAvailable  = Main(content.size, axis);
        const f32 crossAvailable = Cross(content.size, axis);

        std::vector<u32> kids;
        kids.reserve(node.children.size());
        for (u32 child : node.children) {
            if (m_Nodes[child].excluded) { m_Nodes[child].rect = Rect{}; continue; }
            kids.push_back(child);
        }
        const u32 total = static_cast<u32>(kids.size());
        if (total == 0) return;

        // Resolve every child's main extent first. Fill children take no space of their own here;
        // they divide whatever is left on the line they end up on.
        std::vector<f32> mainSizes(total, 0.0f);
        std::vector<f32> fillWeights(total, 0.0f);
        for (u32 i = 0; i < total; ++i) {
            const Node& child = m_Nodes[kids[i]];
            const Size& size = MainSize(child.style, axis);
            switch (size.mode) {
                case SizeMode::Fixed:   mainSizes[i] = size.value; break;
                case SizeMode::Percent: mainSizes[i] = mainAvailable * size.value; break;
                case SizeMode::Hug:     mainSizes[i] = Main(child.measured, axis); break;
                case SizeMode::Fill:
                    fillWeights[i] = std::max(size.value, 0.0f);
                    // For line breaking a Fill child counts as its measured size; it would
                    // otherwise be zero-width and every one of them would pile onto one line.
                    mainSizes[i] = Main(child.measured, axis);
                    break;
            }
        }

        // Split into lines. Without wrap that is exactly one line containing everything.
        struct Line { u32 first = 0, count = 0; f32 mainUsed = 0.0f; f32 fillWeight = 0.0f; f32 cross = 0.0f; };
        std::vector<Line> lines;

        if (!style.wrap || !std::isfinite(mainAvailable)) {
            Line line{ 0, total, 0.0f, 0.0f, 0.0f };
            for (u32 i = 0; i < total; ++i) {
                line.mainUsed += fillWeights[i] > 0.0f ? 0.0f : mainSizes[i];
                line.fillWeight += fillWeights[i];
            }
            lines.push_back(line);
        } else {
            Line line{ 0, 0, 0.0f, 0.0f, 0.0f };
            for (u32 i = 0; i < total; ++i) {
                const f32 withGap = line.count > 0 ? style.gap : 0.0f;
                if (line.count > 0 && line.mainUsed + withGap + mainSizes[i] > mainAvailable) {
                    lines.push_back(line);
                    line = Line{ i, 0, 0.0f, 0.0f, 0.0f };
                }
                line.mainUsed += (line.count > 0 ? style.gap : 0.0f) + mainSizes[i];
                line.fillWeight += fillWeights[i];
                ++line.count;
            }
            lines.push_back(line);
        }

        // Cross extent of each line, and the offset it starts at.
        f32 crossCursor = 0.0f;
        for (auto& line : lines) {
            for (u32 i = line.first; i < line.first + line.count; ++i) {
                const Node& child = m_Nodes[kids[i]];
                const Size& crossSize = CrossSize(child.style, axis);
                const f32 extent = crossSize.mode == SizeMode::Fixed   ? crossSize.value
                                 : crossSize.mode == SizeMode::Percent ? crossAvailable * crossSize.value
                                 : Cross(child.measured, axis);
                line.cross = std::max(line.cross, extent);
            }
            // A single line owns the whole cross extent, so Fill and Stretch behave as before.
            if (lines.size() == 1) line.cross = crossAvailable;
        }

        for (const auto& line : lines) {
            const f32 gapTotal = line.count > 1 ? style.gap * static_cast<f32>(line.count - 1) : 0.0f;
            const f32 leftover = std::max(mainAvailable - line.mainUsed - gapTotal, 0.0f);

            std::vector<f32> resolved(line.count, 0.0f);
            for (u32 k = 0; k < line.count; ++k) {
                const u32 i = line.first + k;
                resolved[k] = line.fillWeight > 0.0f && fillWeights[i] > 0.0f
                            ? leftover * (fillWeights[i] / line.fillWeight)
                            : mainSizes[i];
            }

            // A line that does not fit gives the overflow back, proportionally to what each child
            // asked for — `flex-shrink: 1`. A stated size becomes a preference rather than a
            // promise, and `minSize` is the promise: setting it to the width is `flex-shrink: 0`.
            //
            // Asked for rather than assumed, unlike CSS. A scroll container full of fixed-height
            // rows is a flex container too, and shrinking there squeezes every row to fit the box
            // instead of letting the box scroll — which is exactly the surprise every web developer
            // has met and fixed with `flex-shrink: 0` on everything.
            if (style.shrink && std::isfinite(mainAvailable)) {
                f32 wanted = gapTotal;
                for (const f32 size : resolved) wanted += size;

                // Each pass freezes whatever hit its floor and shares the rest out again; a child
                // pinned at its minimum cannot absorb any more, so what it refused has to go
                // somewhere. Bounded by the child count, which is when everything is frozen.
                std::vector<bool> frozen(line.count, false);
                for (u32 pass = 0; pass < line.count && wanted > mainAvailable + 0.01f; ++pass) {
                    f32 shrinkable = 0.0f;
                    for (u32 k = 0; k < line.count; ++k)
                        if (!frozen[k]) shrinkable += resolved[k];
                    if (shrinkable <= 0.0f) break;

                    const f32 scale = std::max(1.0f - (wanted - mainAvailable) / shrinkable, 0.0f);
                    bool froze = false;
                    for (u32 k = 0; k < line.count; ++k) {
                        if (frozen[k]) continue;
                        const f32 floor_ = Main(m_Nodes[kids[line.first + k]].style.minSize, axis);
                        const f32 shrunk = resolved[k] * scale;
                        if (shrunk < floor_) {
                            wanted -= resolved[k] - floor_;
                            resolved[k] = floor_;
                            frozen[k] = true;
                            froze = true;
                        } else {
                            wanted -= resolved[k] - shrunk;
                            resolved[k] = shrunk;
                        }
                    }
                    if (!froze) break;
                }
            }

            const f32 slack = line.fillWeight > 0.0f ? 0.0f : leftover;
            f32 cursor = 0.0f;
            f32 spacing = style.gap;
            switch (style.justify) {
                case Justify::Start:                                  break;
                case Justify::Center: cursor = slack * 0.5f;          break;
                case Justify::End:    cursor = slack;                 break;
                case Justify::SpaceBetween:
                    if (line.count > 1) spacing += slack / static_cast<f32>(line.count - 1);
                    break;
                case Justify::SpaceAround:
                    if (line.count > 0) {
                        const f32 unit = slack / static_cast<f32>(line.count);
                        cursor = unit * 0.5f;
                        spacing += unit;
                    }
                    break;
                case Justify::SpaceEvenly:
                    if (line.count > 0) {
                        const f32 unit = slack / static_cast<f32>(line.count + 1);
                        cursor = unit;
                        spacing += unit;
                    }
                    break;
            }

            for (u32 k = 0; k < line.count; ++k) {
                const u32 i = line.first + k;
                Node& child = m_Nodes[kids[i]];
                const Size& crossSize = CrossSize(child.style, axis);

                f32 crossExtent = 0.0f;
                switch (crossSize.mode) {
                    case SizeMode::Fixed:   crossExtent = crossSize.value; break;
                    case SizeMode::Percent: crossExtent = crossAvailable * crossSize.value; break;
                    case SizeMode::Fill:    crossExtent = line.cross; break;
                    case SizeMode::Hug:
                        // Stretch spans a hugging child across its line; an explicitly sized child
                        // is left alone, because a stated size outranks an alignment.
                        crossExtent = (style.align == Align::Stretch) ? line.cross
                                                                      : Cross(child.measured, axis);
                        break;
                }

                f32 crossOffset = 0.0f;
                if (crossSize.mode != SizeMode::Fill) {
                    switch (style.align) {
                        case Align::Start:   break;
                        case Align::Center:  crossOffset = (line.cross - crossExtent) * 0.5f; break;
                        case Align::End:     crossOffset = line.cross - crossExtent; break;
                        case Align::Stretch: break;
                    }
                }

                const Vec2 size = Compose(resolved[k], crossExtent, axis);
                const Vec2 pos  = Compose(cursor, crossCursor + crossOffset, axis);
                Arrange(kids[i], Rect{ content.pos + pos, Clamp(child, AspectFit(child, size)) });

                cursor += resolved[k] + spacing;
            }

            crossCursor += line.cross + style.gap;
        }
    }

    void LayoutTree::GridTracks(const LayoutStyle& style, f32 available, u32& columns,
                                f32& track) const {
        if (style.columns > 0) {
            columns = style.columns;
        } else {
            // As many as fit at the minimum, never fewer than one — a container narrower than one
            // column still has to put the column somewhere.
            const f32 minimum = std::max(style.minColumn, 1.0f);
            const f32 room = std::isfinite(available) ? available : minimum;
            columns = static_cast<u32>(std::max(1.0f, std::floor((room + style.gap)
                                                                 / (minimum + style.gap))));
        }
        const f32 gaps = style.gap * static_cast<f32>(columns - 1);
        track = std::isfinite(available) ? std::max((available - gaps) / static_cast<f32>(columns), 0.0f)
                                         : std::max(style.minColumn, 1.0f);
    }

    // Equal columns, filled row by row, each row as tall as its tallest cell. A cell is a track
    // wide unless the child asked for a size of its own, in which case it is placed inside the cell
    // the same way a stack aligns a child across its axis.
    void LayoutTree::ArrangeGrid(Node& node, Rect content) {
        const LayoutStyle& style = node.style;

        std::vector<u32> kids;
        kids.reserve(node.children.size());
        for (u32 child : node.children) {
            if (m_Nodes[child].excluded) { m_Nodes[child].rect = Rect{}; continue; }
            kids.push_back(child);
        }
        if (kids.empty()) return;

        u32 columns = 1;
        f32 track = 0.0f;
        GridTracks(style, content.size.x, columns, track);
        const f32 rowGap = style.rowGap > 0.0f ? style.rowGap : style.gap;

        f32 y = content.pos.y;
        for (std::size_t first = 0; first < kids.size(); first += columns) {
            const std::size_t last = std::min(first + columns, kids.size());

            // The row's height: the tallest cell in it, measured at the track width it will get.
            f32 rowHeight = 0.0f;
            for (std::size_t i = first; i < last; ++i) {
                Node& child = m_Nodes[kids[i]];
                const Size& height = child.style.height;
                f32 wanted = child.measured.y;
                if (height.mode == SizeMode::Fixed) wanted = height.value;
                else if (height.mode == SizeMode::Percent && std::isfinite(content.size.y))
                    wanted = content.size.y * height.value;
                rowHeight = std::max(rowHeight, wanted);
            }

            for (std::size_t i = first; i < last; ++i) {
                Node& child = m_Nodes[kids[i]];
                const f32 x = content.pos.x
                            + static_cast<f32>(i - first) * (track + style.gap);

                Vec2 size{ track, rowHeight };
                const Size& width = child.style.width;
                if (width.mode == SizeMode::Fixed) size.x = width.value;
                else if (width.mode == SizeMode::Percent) size.x = track * width.value;
                else if (width.mode == SizeMode::Hug) size.x = std::min(child.measured.x, track);

                const Size& height = child.style.height;
                if (height.mode == SizeMode::Fixed) size.y = height.value;
                else if (height.mode == SizeMode::Percent && std::isfinite(content.size.y))
                    size.y = content.size.y * height.value;
                else if (height.mode == SizeMode::Hug) size.y = std::min(child.measured.y, rowHeight);

                size = Clamp(child, AspectFit(child, size));

                // Where a cell smaller than its track sits in it. `align` is the cross axis of a
                // row, so it places the child vertically; `justify` places it horizontally, which
                // keeps the two words meaning the same thing they mean in a stack.
                Vec2 at{ x, y };
                const f32 slackX = track - size.x;
                const f32 slackY = rowHeight - size.y;
                switch (style.justify) {
                    case Justify::Center: at.x += slackX * 0.5f; break;
                    case Justify::End:    at.x += slackX; break;
                    default: break;
                }
                switch (style.align) {
                    case Align::Center: at.y += slackY * 0.5f; break;
                    case Align::End:    at.y += slackY; break;
                    default: break;
                }

                Arrange(kids[i], Rect{ at, size });
            }

            y += rowHeight + rowGap;
        }
    }

    void LayoutTree::ArrangeAbsolute(Node& node, Rect content) {
        for (u32 index : node.children) {
            Node& child = m_Nodes[index];
            if (child.excluded) { child.rect = Rect{}; continue; }
            const LayoutStyle& cs = child.style;

            auto Resolve = [&](Constraint constraint, f32 parentExtent, f32 start, f32 end,
                               const Size& size, f32 measured) -> std::pair<f32, f32> {
                f32 extent = measured;
                switch (size.mode) {
                    case SizeMode::Fixed:   extent = size.value; break;
                    case SizeMode::Percent: extent = parentExtent * size.value; break;
                    case SizeMode::Fill: {
                        // Fill on an absolutely-positioned child means "reach the far edge from
                        // where you are". Handing it the parent's whole extent instead hangs it off
                        // the end by exactly its own offset, which is what a widget dropped onto a
                        // canvas at x=640 did: it drew 1280 wide, 640 of it outside the screen.
                        const f32 taken = (constraint == Constraint::End) ? end
                                        : (constraint == Constraint::Center
                                        || constraint == Constraint::Scale) ? 0.0f
                                        : start;
                        extent = std::max(parentExtent - taken, 0.0f);
                        break;
                    }
                    case SizeMode::Hug:     break;
                }

                switch (constraint) {
                    case Constraint::Start:  return { start, extent };
                    case Constraint::End:    return { parentExtent - end - extent, extent };
                    case Constraint::StartEnd:
                        // Both edges pinned: the child stretches, and its own size mode is
                        // overruled — that is the whole point of pinning both edges.
                        return { start, std::max(parentExtent - start - end, 0.0f) };
                    case Constraint::Center:
                        return { (parentExtent - extent) * 0.5f + start, extent };
                    case Constraint::Scale:
                        // Offsets and size are stored as fractions of the parent.
                        return { parentExtent * start, parentExtent * (size.mode == SizeMode::Fixed
                                                                       ? size.value / std::max(parentExtent, 1.0f)
                                                                       : size.value) };
                }
                return { start, extent };
            };

            const auto [x, w] = Resolve(cs.constraintX, content.size.x, cs.offsetStart.x,
                                        cs.offsetEnd.x, cs.width, child.measured.x);
            const auto [y, h] = Resolve(cs.constraintY, content.size.y, cs.offsetStart.y,
                                        cs.offsetEnd.y, cs.height, child.measured.y);

            Arrange(index, Rect{ content.pos + Vec2{ x, y },
                                 Clamp(child, AspectFit(child, Vec2{ w, h })) });
        }
    }

    void LayoutTree::Arrange(u32 index, Rect box) {
        Node& node = m_Nodes[index];
        node.rect = box;
        if (node.children.empty()) return;

        const Rect content{
            { node.style.padding.left, node.style.padding.top },
            { std::max(box.size.x - node.style.padding.Horizontal(), 0.0f),
              std::max(box.size.y - node.style.padding.Vertical(),   0.0f) },
        };

        switch (node.style.mode) {
            case LayoutMode::Stack: ArrangeStack(node, content); break;
            case LayoutMode::Grid:  ArrangeGrid(node, content); break;
            default:                ArrangeAbsolute(node, content); break;
        }
    }

    void LayoutTree::Compute(u32 root, Vec2 available) {
        if (m_Nodes.empty()) return;

        Solve(root, available);

        // A wrapping text node, or a reflowing container, measured against the room its parent
        // *offered*, which is not always the width it ended up with. Re-measure the ones that
        // moved and solve once more — two
        // passes, not a fixed point, because a third pass has never yet changed an answer and an
        // unbounded loop in layout is how a UI freezes.
        bool changed = false;
        for (Node& node : m_Nodes) {
            if (node.measuredAt < 0.0f) continue;
            const f32 finalWidth = std::max(node.rect.size.x - node.style.padding.Horizontal(), 0.0f);
            if (std::abs(finalWidth - node.measuredAt) <= 0.5f) continue;
            node.forcedWidth = finalWidth;
            changed = true;
        }
        if (!changed) return;
        Solve(root, available);
        for (Node& node : m_Nodes) node.forcedWidth = -1.0f;
    }

    void LayoutTree::Solve(u32 root, Vec2 available) {
        Measure(root, available);

        const LayoutStyle& style = m_Nodes[root].style;
        Vec2 size = m_Nodes[root].measured;
        // The root has no parent to fill, so Fill and Percent resolve against the box it was given.
        if (style.width.mode  == SizeMode::Fill)    size.x = available.x;
        if (style.height.mode == SizeMode::Fill)    size.y = available.y;
        if (style.width.mode  == SizeMode::Percent) size.x = available.x * style.width.value;
        if (style.height.mode == SizeMode::Percent) size.y = available.y * style.height.value;

        Arrange(root, Rect{ { 0.0f, 0.0f }, Clamp(m_Nodes[root], size) });
    }

}
