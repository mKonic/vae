#pragma once

#include "vae/base/Math.h"

#include <vector>

namespace vae::vector {

    // A 2D affine transform, in SVG's own layout: [a c e / b d f]. Kept here rather than reaching
    // for glm's mat3 because SVG's `matrix(a b c d e f)` maps to it one-to-one, and a transform
    // stack that reads differently from the file it came from is a transform stack with bugs in it.
    struct Affine {
        f32 a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

        Vec2 Apply(Vec2 p) const { return { a * p.x + c * p.y + e, b * p.x + d * p.y + f }; }
        // No translation: for anything that is a direction or a length rather than a place.
        Vec2 ApplyVector(Vec2 v) const { return { a * v.x + c * v.y, b * v.x + d * v.y }; }

        // How much this transform grows a length, on average. A stroke drawn through a squashed
        // transform has no one width, and this is the number everyone means by "the" width.
        f32 Scale() const;

        // `this` first, then `outer` — so a child's own transform composes onto its parent's the
        // way nesting reads in the file.
        Affine Then(const Affine& outer) const;

        static Affine Translate(Vec2 d) { return { 1.0f, 0.0f, 0.0f, 1.0f, d.x, d.y }; }
        static Affine Scaling(Vec2 s)   { return { s.x, 0.0f, 0.0f, s.y, 0.0f, 0.0f }; }
        static Affine Rotate(f32 degrees);
        static Affine SkewX(f32 degrees);
        static Affine SkewY(f32 degrees);
    };

    enum class FillRule : u8 { NonZero, EvenOdd };

    // A ring of straight segments — what the rasterizer wants, and what a curve becomes once it has
    // been told how much error is acceptable.
    struct Contour {
        std::vector<Vec2> points;
        bool closed = true;
    };

    // A shape as it was authored: verbs and control points, curves intact. Flattening is deferred
    // because how fine is fine enough depends on how big the thing will be drawn, and a path
    // flattened at authoring size looks like a polygon at four times the zoom.
    class Path {
    public:
        void MoveTo(Vec2 p);
        void LineTo(Vec2 p);
        void QuadTo(Vec2 control, Vec2 to);
        void CubicTo(Vec2 c1, Vec2 c2, Vec2 to);
        // SVG's elliptical arc, converted to cubics on the spot. Nobody downstream wants to know
        // about arcs, and the conversion is exact enough that nobody can tell.
        void ArcTo(Vec2 radii, f32 xRotationDegrees, bool largeArc, bool sweep, Vec2 to);
        void Close();

        void Clear();
        bool Empty() const { return m_Verbs.empty(); }
        Vec2 Current() const { return m_Current; }
        // Where the last MoveTo put the pen — where Close returns to.
        Vec2 Start() const { return m_Start; }
        // The last cubic's second control point, reflected — what SVG's S and T commands mean by
        // "the same curvature continuing". Equals `Current` when the previous verb was not a curve.
        Vec2 ReflectedControl(bool cubic) const;

        // Straight segments in the transformed space, with no segment deviating from the true
        // curve by more than `tolerance` (in that same space). Open contours come back open: a
        // stroke has to know it should not join the last point to the first.
        std::vector<Contour> Flatten(const Affine& transform, f32 tolerance = 0.2f) const;

        // The box around the control points — a superset of the real bounds, which is the honest
        // cheap answer and the right one for deciding how big a raster to allocate.
        Rect ControlBounds() const;

    private:
        enum class Verb : u8 { Move, Line, Quad, Cubic, Close };

        void Push(Verb verb, std::initializer_list<Vec2> points);

        std::vector<Verb> m_Verbs;
        std::vector<Vec2> m_Points;
        Vec2 m_Current{ 0.0f, 0.0f };
        Vec2 m_Start{ 0.0f, 0.0f };
        bool m_LastWasCubic = false;
        bool m_LastWasQuad = false;
    };

}
