#include "vaepch.h"
#include "vae/vector/Raster.h"

#include <algorithm>
#include <cmath>

namespace vae::vector {

    namespace {

        // Sub-scanlines per pixel row. Horizontal coverage is exact — a span's overlap with a pixel
        // is arithmetic — so this number is only the vertical resolution, and eight steps is the
        // point where a near-horizontal edge stops showing stairs at icon sizes.
        constexpr int kSamples = 8;

        struct Edge {
            f32 x0, y0, x1, y1;      // y0 < y1 always
            f32 slope;               // dx/dy
            int winding;             // +1 if the original edge pointed down, -1 if up
        };

        void AddEdge(std::vector<Edge>& edges, Vec2 a, Vec2 b) {
            if (std::abs(a.y - b.y) < 1e-9f) return;      // horizontal edges cross nothing
            const int winding = a.y < b.y ? 1 : -1;
            if (a.y > b.y) std::swap(a, b);
            edges.push_back({ a.x, a.y, b.x, b.y, (b.x - a.x) / (b.y - a.y), winding });
        }

        struct Crossing { f32 x; int winding; };

    }

    Mask Fill(const std::vector<Contour>& contours, FillRule rule, u32 width, u32 height) {
        Mask mask;
        if (width == 0 || height == 0) return mask;

        std::vector<Edge> edges;
        for (const Contour& contour : contours) {
            const std::size_t count = contour.points.size();
            if (count < 2) continue;
            for (std::size_t i = 0; i + 1 < count; ++i)
                AddEdge(edges, contour.points[i], contour.points[i + 1]);
            // Filling an open contour means filling the shape it would be if it were closed.
            AddEdge(edges, contour.points[count - 1], contour.points[0]);
        }
        if (edges.empty()) return mask;

        mask.width = width;
        mask.height = height;
        mask.coverage.assign(static_cast<std::size_t>(width) * height, 0);

        std::ranges::sort(edges, [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });

        // Only the rows any edge actually touches: an icon is mostly empty space, and rows nothing
        // crosses cost nothing to skip.
        f32 lowest = edges.front().y0, highest = edges.front().y1;
        for (const Edge& edge : edges) {
            lowest = std::min(lowest, edge.y0);
            highest = std::max(highest, edge.y1);
        }
        const int firstRow = std::max(0, static_cast<int>(std::floor(lowest)));
        const int lastRow = std::min(static_cast<int>(height) - 1,
                                     static_cast<int>(std::ceil(highest)));
        if (firstRow > lastRow) return mask;

        std::vector<f32> row(width, 0.0f);
        std::vector<const Edge*> active;
        std::vector<Crossing> crossings;
        std::size_t next = 0;

        // Wind the edge cursor forward to the first row we care about.
        while (next < edges.size() && edges[next].y1 <= static_cast<f32>(firstRow)) ++next;

        const f32 share = 1.0f / static_cast<f32>(kSamples);

        for (int y = firstRow; y <= lastRow; ++y) {
            std::fill(row.begin(), row.end(), 0.0f);

            const f32 rowTop = static_cast<f32>(y);
            const f32 rowBottom = rowTop + 1.0f;
            while (next < edges.size() && edges[next].y0 < rowBottom) active.push_back(&edges[next++]);
            std::erase_if(active, [&](const Edge* edge) { return edge->y1 <= rowTop; });
            if (active.empty()) continue;

            for (int sample = 0; sample < kSamples; ++sample) {
                const f32 scanY = rowTop + (static_cast<f32>(sample) + 0.5f) * share;

                crossings.clear();
                for (const Edge* edge : active) {
                    if (scanY < edge->y0 || scanY >= edge->y1) continue;
                    crossings.push_back({ edge->x0 + (scanY - edge->y0) * edge->slope,
                                          edge->winding });
                }
                if (crossings.size() < 2) continue;
                std::ranges::sort(crossings, [](const Crossing& a, const Crossing& b) {
                    return a.x < b.x;
                });

                int winding = 0;
                for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
                    winding += crossings[i].winding;
                    const bool inside = rule == FillRule::NonZero ? winding != 0
                                                                  : (winding & 1) != 0;
                    if (!inside) continue;

                    const f32 spanStart = std::max(crossings[i].x, 0.0f);
                    const f32 spanEnd = std::min(crossings[i + 1].x, static_cast<f32>(width));
                    if (spanEnd <= spanStart) continue;

                    // Exact horizontal coverage: the two end pixels get their partial overlap and
                    // everything between them gets the whole sub-scanline's share.
                    const int firstPixel = static_cast<int>(spanStart);
                    const int lastPixel = std::min(static_cast<int>(spanEnd),
                                                   static_cast<int>(width) - 1);
                    if (firstPixel == lastPixel) {
                        row[firstPixel] += (spanEnd - spanStart) * share;
                        continue;
                    }
                    row[firstPixel] += (static_cast<f32>(firstPixel + 1) - spanStart) * share;
                    for (int x = firstPixel + 1; x < lastPixel; ++x) row[x] += share;
                    row[lastPixel] += (spanEnd - static_cast<f32>(lastPixel)) * share;
                }
            }

            u8* out = mask.coverage.data() + static_cast<std::size_t>(y) * width;
            for (u32 x = 0; x < width; ++x)
                out[x] = static_cast<u8>(std::clamp(row[x], 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        return mask;
    }

    // -------------------------------------------------------------------------------- stroking

    namespace {

        f32 SignedArea(const std::vector<Vec2>& points) {
            f32 total = 0.0f;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const Vec2 a = points[i];
                const Vec2 b = points[(i + 1) % points.size()];
                total += a.x * b.y - b.x * a.y;
            }
            return total * 0.5f;
        }

        // Every piece of a stroke outline is unioned by the nonzero rule, and nonzero only unions
        // shapes that agree about which way is out. Rather than reason about turn direction at
        // every join, each piece is simply wound the same way after the fact.
        void Emit(std::vector<Contour>& out, std::vector<Vec2> points) {
            if (points.size() < 3) return;
            if (SignedArea(points) < 0.0f) std::ranges::reverse(points);
            out.push_back({ std::move(points), true });
        }

        Vec2 Normalized(Vec2 v) {
            const f32 length = std::sqrt(v.x * v.x + v.y * v.y);
            return length > 1e-9f ? Vec2{ v.x / length, v.y / length } : Vec2{ 0.0f, 0.0f };
        }

        void EmitDisc(std::vector<Contour>& out, Vec2 centre, f32 radius) {
            // Enough sides that the flat spots are under a third of a pixel at this radius, and
            // never so many that a hairline join costs a hundred points.
            const int sides = std::clamp(static_cast<int>(std::ceil(radius * 2.0f)) + 8, 8, 64);
            std::vector<Vec2> points;
            points.reserve(sides);
            for (int i = 0; i < sides; ++i) {
                const f32 angle = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(sides);
                points.push_back({ centre.x + std::cos(angle) * radius,
                                   centre.y + std::sin(angle) * radius });
            }
            Emit(out, std::move(points));
        }

    }

    std::vector<Contour> Stroke(const std::vector<Contour>& contours, f32 width,
                                LineJoin join, LineCap cap, f32 miterLimit) {
        std::vector<Contour> out;
        const f32 half = std::max(width, 0.01f) * 0.5f;

        for (const Contour& contour : contours) {
            // Duplicate points make a zero-length segment, which has no direction and therefore no
            // normal — they are dropped rather than allowed to produce a NaN.
            std::vector<Vec2> points;
            for (const Vec2 p : contour.points) {
                if (points.empty()) { points.push_back(p); continue; }
                const Vec2 d = p - points.back();
                if (d.x * d.x + d.y * d.y > 1e-12f) points.push_back(p);
            }
            if (contour.closed && points.size() > 1) {
                const Vec2 d = points.front() - points.back();
                if (d.x * d.x + d.y * d.y <= 1e-12f) points.pop_back();
            }

            if (points.size() < 2) {
                // A degenerate contour still draws under a round cap: that is how a dot is spelled.
                if (points.size() == 1 && cap == LineCap::Round) EmitDisc(out, points[0], half);
                continue;
            }

            const std::size_t count = points.size();
            const std::size_t segments = contour.closed ? count : count - 1;

            for (std::size_t i = 0; i < segments; ++i) {
                const Vec2 a = points[i];
                const Vec2 b = points[(i + 1) % count];
                const Vec2 dir = Normalized(b - a);
                const Vec2 normal{ -dir.y * half, dir.x * half };

                Vec2 from = a, to = b;
                if (!contour.closed && cap == LineCap::Square) {
                    if (i == 0) from = from - dir * half;
                    if (i + 1 == segments) to = to + dir * half;
                }
                Emit(out, { from + normal, to + normal, to - normal, from - normal });
            }

            // Joins. A disc at the vertex covers the wedge whichever way the path turns, and a
            // miter adds the point back on top of it when the corner is sharp enough to deserve one.
            const std::size_t joints = contour.closed ? count : count - 2;
            for (std::size_t j = 0; j < joints; ++j) {
                const std::size_t index = contour.closed ? j : j + 1;
                const Vec2 vertex = points[index];
                const Vec2 previous = points[(index + count - 1) % count];
                const Vec2 following = points[(index + 1) % count];
                const Vec2 in = Normalized(vertex - previous);
                const Vec2 outward = Normalized(following - vertex);

                if (join != LineJoin::Bevel) EmitDisc(out, vertex, half);
                else {
                    Emit(out, { vertex,
                                vertex + Vec2{ -in.y * half, in.x * half },
                                vertex + Vec2{ -outward.y * half, outward.x * half } });
                    Emit(out, { vertex,
                                vertex - Vec2{ -in.y * half, in.x * half },
                                vertex - Vec2{ -outward.y * half, outward.x * half } });
                }

                if (join != LineJoin::Miter) continue;
                // The outer side is the one the turn opens away from; both normals are taken there.
                const f32 turn = in.x * outward.y - in.y * outward.x;
                if (std::abs(turn) < 1e-6f) continue;
                const f32 side = turn > 0.0f ? -1.0f : 1.0f;
                const Vec2 n0{ -in.y * side, in.x * side };
                const Vec2 n1{ -outward.y * side, outward.x * side };
                const Vec2 bisector = Normalized(n0 + n1);
                const f32 alignment = bisector.x * n0.x + bisector.y * n0.y;
                if (alignment < 1e-4f) continue;
                const f32 length = half / alignment;
                if (length > miterLimit * half) continue;      // too sharp: the disc is the bevel
                Emit(out, { vertex + n0 * half, vertex + bisector * length, vertex + n1 * half,
                            vertex });
            }

            if (!contour.closed && cap == LineCap::Round) {
                EmitDisc(out, points.front(), half);
                EmitDisc(out, points.back(), half);
            }
        }

        return out;
    }

}
