#pragma once

#include "vae/layout/LayoutTypes.h"

#include <functional>
#include <vector>

namespace vae::layout {

    // A flat, index-addressed tree. Flat because the document model is a flat map of nodes too, and
    // because a bottom-up measure pass over a vector is cache-friendly in a way a pointer-chased
    // tree is not.
    class LayoutTree {
    public:
        static constexpr u32 kInvalid = 0xFFFFFFFFu;

        void Clear();
        u32  Add(const LayoutStyle& style, u32 parent = kInvalid);

        // Content size of a leaf: a text run's measured box, an image's pixel size. Nodes with
        // children derive theirs instead.
        void SetIntrinsic(u32 node, Vec2 size);

        // A leaf whose size depends on how much room it is given — text, chiefly. Called with the
        // content box available to it; an infinite component means "unbounded on that axis".
        using MeasureFn = std::function<Vec2(Vec2 available)>;
        void SetMeasure(u32 node, MeasureFn measure);

        // A hidden node takes no space — not zero size, no space: no gap around it, no line of its
        // own when wrapping. That is Figma's rule for invisible auto-layout children, and it is
        // what lets a dropdown keep its closed menu in the tree without it shoving the layout.
        void SetExcluded(u32 node, bool excluded);
        void SetStyle(u32 node, const LayoutStyle& style);

        // `available` is the box the root is laid out into. Root sizing modes resolve against it,
        // so a Fill root fills the window and a Hug root shrinks to its content.
        void Compute(u32 root, Vec2 available);

        const Rect& NodeRect(u32 node) const { return m_Nodes[node].rect; }   // parent space
        Rect  AbsoluteRect(u32 node) const;                                    // root space
        Vec2  MeasuredSize(u32 node) const { return m_Nodes[node].measured; }
        const LayoutStyle& Style(u32 node) const { return m_Nodes[node].style; }
        u32   Parent(u32 node) const { return m_Nodes[node].parent; }
        const std::vector<u32>& Children(u32 node) const { return m_Nodes[node].children; }
        u32   NodeCount() const { return static_cast<u32>(m_Nodes.size()); }

    private:
        struct Node {
            LayoutStyle style;
            std::vector<u32> children;
            u32  parent = kInvalid;
            Vec2 intrinsic{ 0.0f, 0.0f };
            MeasureFn measure;
            f32  measuredAt = -1.0f;       // the width `measure` was last asked about
            bool excluded = false;
            f32  forcedWidth = -1.0f;      // pass 2 only: measure at the width arrange settled on
            Vec2 measured{ 0.0f, 0.0f };   // hug size, from the measure pass
            Rect rect{};                   // final, in parent space
        };

        // Pass 1, bottom-up: what does this node need if nothing constrains it?
        Vec2 Measure(u32 node, Vec2 available);
        // Pass 2, top-down: given a settled box, place the children.
        void Arrange(u32 node, Rect box);
        // The aspect ratio applied once the box is real. Measure can only use it when a size was
        // already known; a node that fills its parent does not know its width until it is arranged,
        // and that is exactly the node an aspect ratio is for.
        Vec2 AspectFit(const Node& node, Vec2 size) const;
        void ArrangeStack(Node& node, Rect content);
        void ArrangeGrid(Node& node, Rect content);
        // Track count and width for a grid inside `available` px of content box. Shared by measure
        // and arrange so the two cannot disagree about how many columns there are.
        void GridTracks(const LayoutStyle& style, f32 available, u32& columns, f32& track) const;
        void ArrangeAbsolute(Node& node, Rect content);

        void Solve(u32 root, Vec2 available);
        Vec2 Clamp(const Node& node, Vec2 size) const;
        Vec2 ApplyAspect(const Node& node, Vec2 size, bool widthKnown, bool heightKnown) const;

        std::vector<Node> m_Nodes;
    };

}
