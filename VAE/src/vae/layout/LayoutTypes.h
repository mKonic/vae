#pragma once

#include "vae/base/Base.h"
#include "vae/base/Math.h"

#include <limits>

namespace vae::layout {

    // How a node arranges its CHILDREN.
    //   Absolute — children are placed by their own offsets and edge constraints (Figma's default,
    //              and what direct manipulation on a canvas produces).
    //   Stack    — children flow along one axis with gap/padding/alignment (Figma's auto-layout).
    //   Grid     — equal columns, filled row by row. Not composable out of stacks: a card gallery,
    //              a form's label column and a toolbar of tiles all need cells that line up down the
    //              page as well as across it, which a stack of stacks only manages by accident.
    // Those three, not CSS in full: they are the vocabulary a designer actually works in, and the
    // subset is small enough to specify exactly and test exhaustively.
    enum class LayoutMode : u8 { Absolute, Stack, Grid };

    enum class Axis : u8 { Row, Column };

    enum class SizeMode : u8 {
        Fixed,      // exactly this many pixels
        Hug,        // as large as the content needs
        Fill,       // share whatever the parent has left, by weight
        Percent,    // a fraction of the parent's content box
    };

    struct Size {
        SizeMode mode = SizeMode::Hug;
        f32 value = 0.0f;      // px (Fixed) · weight (Fill) · fraction 0..1 (Percent)

        static constexpr Size Px(f32 px)          { return { SizeMode::Fixed, px }; }
        static constexpr Size Hug()               { return { SizeMode::Hug, 0.0f }; }
        static constexpr Size Fill(f32 w = 1.0f)  { return { SizeMode::Fill, w }; }
        static constexpr Size Percent(f32 f)      { return { SizeMode::Percent, f }; }

        bool operator==(const Size&) const = default;
    };

    // Cross-axis placement of children in a stack.
    enum class Align : u8 { Start, Center, End, Stretch };

    // Main-axis distribution of children in a stack.
    enum class Justify : u8 { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };

    // How an absolutely-positioned child reacts when its parent resizes. Figma's constraint model.
    //   Start    — pinned to the left/top edge
    //   End      — pinned to the right/bottom edge
    //   StartEnd — both edges pinned, so the child stretches
    //   Center   — keeps its offset from the centre
    //   Scale    — offset and size stay proportional to the parent
    enum class Constraint : u8 { Start, End, StartEnd, Center, Scale };

    inline constexpr f32 kUnbounded = std::numeric_limits<f32>::infinity();

    struct LayoutStyle {
        LayoutMode mode = LayoutMode::Absolute;
        Axis    axis = Axis::Column;
        Size    width = Size::Hug();
        Size    height = Size::Hug();
        Edges   padding{};
        f32     gap = 0.0f;
        Align   align = Align::Start;
        Justify justify = Justify::Start;
        bool    wrap = false;
        // Whether a line that does not fit gives the overflow back, proportionally to what each
        // child asked for. This is `flex-shrink: 1`, and it is off by default where CSS has it on:
        // a scroll container full of fixed-height rows is the case where shrinking silently
        // destroys the thing being built, and it is the case CSS is famous for getting wrong.
        // `minSize` is the floor a child cannot be squeezed past.
        bool    shrink = false;

        // --- grid ---------------------------------------------------------------------------
        // How many columns. Zero means as many as fit at `minColumn`, which is the version that
        // survives a resize — a gallery that reflows rather than one that overflows.
        u16 columns = 0;
        f32 minColumn = 160.0f;
        // Vertical gap between rows. Zero means "the same as `gap`", so the common case says it once.
        f32 rowGap = 0.0f;

        Vec2 minSize{ 0.0f, 0.0f };
        Vec2 maxSize{ kUnbounded, kUnbounded };
        // width / height. When set, whichever axis is not otherwise determined follows from it.
        f32  aspectRatio = 0.0f;

        // Absolute positioning, relative to the parent's content box.
        //   offsetStart — distance from the left/top edge   (Start, StartEnd, Center offset, Scale fraction)
        //   offsetEnd   — distance from the right/bottom edge (End, StartEnd)
        Vec2 offsetStart{ 0.0f, 0.0f };
        Vec2 offsetEnd{ 0.0f, 0.0f };
        Constraint constraintX = Constraint::Start;
        Constraint constraintY = Constraint::Start;

        bool operator==(const LayoutStyle&) const = default;
    };

}
