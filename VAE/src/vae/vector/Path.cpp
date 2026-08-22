#include "vaepch.h"
#include "vae/vector/Path.h"

#include <cmath>

namespace vae::vector {

    namespace {
        constexpr f32 kPi = 3.14159265358979323846f;
        constexpr f32 kDegrees = kPi / 180.0f;
    }

    f32 Affine::Scale() const {
        // The geometric mean of the two axis lengths: it is the factor that preserves area, which
        // is the one that makes a circle stroked through a squashed transform look right.
        const f32 determinant = std::abs(a * d - b * c);
        return std::sqrt(std::max(determinant, 0.0f));
    }

    Affine Affine::Then(const Affine& outer) const {
        return { outer.a * a + outer.c * b,
                 outer.b * a + outer.d * b,
                 outer.a * c + outer.c * d,
                 outer.b * c + outer.d * d,
                 outer.a * e + outer.c * f + outer.e,
                 outer.b * e + outer.d * f + outer.f };
    }

    Affine Affine::Rotate(f32 degrees) {
        const f32 s = std::sin(degrees * kDegrees);
        const f32 c = std::cos(degrees * kDegrees);
        return { c, s, -s, c, 0.0f, 0.0f };
    }

    Affine Affine::SkewX(f32 degrees) { return { 1.0f, 0.0f, std::tan(degrees * kDegrees), 1.0f, 0.0f, 0.0f }; }
    Affine Affine::SkewY(f32 degrees) { return { 1.0f, std::tan(degrees * kDegrees), 0.0f, 1.0f, 0.0f, 0.0f }; }

    // --------------------------------------------------------------------------------- building

    void Path::Push(Verb verb, std::initializer_list<Vec2> points) {
        m_Verbs.push_back(verb);
        m_Points.insert(m_Points.end(), points.begin(), points.end());
    }

    void Path::MoveTo(Vec2 p) {
        Push(Verb::Move, { p });
        m_Current = m_Start = p;
        m_LastWasCubic = m_LastWasQuad = false;
    }

    void Path::LineTo(Vec2 p) {
        // A line before any move is a line from the origin in SVG's reading, but every real file
        // opens with an M — seeding the pen keeps a malformed one from drawing from nowhere.
        if (m_Verbs.empty()) MoveTo(p);
        else {
            Push(Verb::Line, { p });
            m_Current = p;
            m_LastWasCubic = m_LastWasQuad = false;
        }
    }

    void Path::QuadTo(Vec2 control, Vec2 to) {
        if (m_Verbs.empty()) MoveTo(m_Current);
        Push(Verb::Quad, { control, to });
        m_Current = to;
        m_LastWasCubic = false;
        m_LastWasQuad = true;
    }

    void Path::CubicTo(Vec2 c1, Vec2 c2, Vec2 to) {
        if (m_Verbs.empty()) MoveTo(m_Current);
        Push(Verb::Cubic, { c1, c2, to });
        m_Current = to;
        m_LastWasCubic = true;
        m_LastWasQuad = false;
    }

    void Path::Close() {
        if (m_Verbs.empty()) return;
        Push(Verb::Close, {});
        m_Current = m_Start;
        m_LastWasCubic = m_LastWasQuad = false;
    }

    void Path::Clear() {
        m_Verbs.clear();
        m_Points.clear();
        m_Current = m_Start = { 0.0f, 0.0f };
        m_LastWasCubic = m_LastWasQuad = false;
    }

    Vec2 Path::ReflectedControl(bool cubic) const {
        if (cubic ? !m_LastWasCubic : !m_LastWasQuad) return m_Current;
        // The control point before the endpoint: cubics store (c1, c2, to), quads (control, to).
        const std::size_t back = m_Points.size();
        const Vec2 control = m_Points[back - (cubic ? 2 : 2)];
        return m_Current + (m_Current - control);
    }

    void Path::ArcTo(Vec2 radii, f32 xRotationDegrees, bool largeArc, bool sweep, Vec2 to) {
        // The endpoint parameterisation SVG uses, turned into the centre one, then into cubics.
        // Straight out of the specification's own implementation notes (F.6.5), because getting
        // this subtly wrong shows up as an icon that is very slightly the wrong shape.
        const Vec2 from = m_Current;
        if (from == to) return;

        f32 rx = std::abs(radii.x), ry = std::abs(radii.y);
        if (rx < 1e-6f || ry < 1e-6f) { LineTo(to); return; }

        const f32 phi = xRotationDegrees * kDegrees;
        const f32 cosPhi = std::cos(phi), sinPhi = std::sin(phi);

        const Vec2 half = (from - to) * 0.5f;
        const Vec2 p1{ cosPhi * half.x + sinPhi * half.y, -sinPhi * half.x + cosPhi * half.y };

        // Radii too small to reach: the specification says scale them up until they just do.
        const f32 lambda = (p1.x * p1.x) / (rx * rx) + (p1.y * p1.y) / (ry * ry);
        if (lambda > 1.0f) {
            const f32 grow = std::sqrt(lambda);
            rx *= grow;
            ry *= grow;
        }

        const f32 numerator = std::max(0.0f, rx * rx * ry * ry - rx * rx * p1.y * p1.y
                                                               - ry * ry * p1.x * p1.x);
        const f32 denominator = rx * rx * p1.y * p1.y + ry * ry * p1.x * p1.x;
        const f32 coefficient = (largeArc == sweep ? -1.0f : 1.0f)
                              * std::sqrt(denominator > 1e-12f ? numerator / denominator : 0.0f);

        const Vec2 c1{ coefficient * rx * p1.y / ry, -coefficient * ry * p1.x / rx };
        const Vec2 centre{ cosPhi * c1.x - sinPhi * c1.y + (from.x + to.x) * 0.5f,
                           sinPhi * c1.x + cosPhi * c1.y + (from.y + to.y) * 0.5f };

        const auto angle = [](Vec2 u, Vec2 v) {
            const f32 dot = u.x * v.x + u.y * v.y;
            const f32 len = std::sqrt((u.x * u.x + u.y * u.y) * (v.x * v.x + v.y * v.y));
            f32 result = std::acos(std::clamp(len > 1e-12f ? dot / len : 1.0f, -1.0f, 1.0f));
            if (u.x * v.y - u.y * v.x < 0.0f) result = -result;
            return result;
        };

        const Vec2 start{ (p1.x - c1.x) / rx, (p1.y - c1.y) / ry };
        const Vec2 end{ (-p1.x - c1.x) / rx, (-p1.y - c1.y) / ry };
        const f32 theta = angle({ 1.0f, 0.0f }, start);
        f32 sweepAngle = angle(start, end);
        if (!sweep && sweepAngle > 0.0f) sweepAngle -= 2.0f * kPi;
        else if (sweep && sweepAngle < 0.0f) sweepAngle += 2.0f * kPi;

        // A cubic approximates at most a quarter turn before the error becomes visible.
        const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweepAngle)
                                                                    / (kPi * 0.5f))));
        const f32 delta = sweepAngle / static_cast<f32>(segments);
        const f32 handle = 4.0f / 3.0f * std::tan(delta * 0.25f);

        f32 at = theta;
        for (int i = 0; i < segments; ++i) {
            const f32 next = at + delta;
            const Vec2 unitA{ std::cos(at), std::sin(at) };
            const Vec2 unitB{ std::cos(next), std::sin(next) };

            const auto onEllipse = [&](Vec2 unit) {
                const Vec2 scaled{ unit.x * rx, unit.y * ry };
                return Vec2{ cosPhi * scaled.x - sinPhi * scaled.y + centre.x,
                             sinPhi * scaled.x + cosPhi * scaled.y + centre.y };
            };
            const auto tangent = [&](Vec2 unit) {
                const Vec2 scaled{ -unit.y * rx, unit.x * ry };
                return Vec2{ cosPhi * scaled.x - sinPhi * scaled.y,
                             sinPhi * scaled.x + cosPhi * scaled.y };
            };

            const Vec2 pointA = onEllipse(unitA);
            const Vec2 pointB = i + 1 == segments ? to : onEllipse(unitB);
            CubicTo(pointA + tangent(unitA) * handle, pointB - tangent(unitB) * handle, pointB);
            at = next;
        }
    }

    // ------------------------------------------------------------------------------- flattening

    namespace {

        // Recursive subdivision, stopping when the control points are within tolerance of the
        // chord. Cheaper than a fixed segment count where it does not matter and finer where it
        // does, which is the whole reason a curve is not stored as a polygon in the first place.
        void FlattenCubic(std::vector<Vec2>& out, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3,
                          f32 tolerance, int depth) {
            const Vec2 chord = p3 - p0;
            const f32 length = std::sqrt(chord.x * chord.x + chord.y * chord.y);
            f32 error = 0.0f;
            if (length < 1e-6f) {
                const Vec2 d1 = p1 - p0, d2 = p2 - p0;
                error = std::max(std::sqrt(d1.x * d1.x + d1.y * d1.y),
                                 std::sqrt(d2.x * d2.x + d2.y * d2.y));
            } else {
                const auto distance = [&](Vec2 p) {
                    return std::abs(chord.x * (p.y - p0.y) - chord.y * (p.x - p0.x)) / length;
                };
                error = std::max(distance(p1), distance(p2));
            }

            if (depth >= 16 || error <= tolerance) { out.push_back(p3); return; }

            const Vec2 p01 = (p0 + p1) * 0.5f, p12 = (p1 + p2) * 0.5f, p23 = (p2 + p3) * 0.5f;
            const Vec2 p012 = (p01 + p12) * 0.5f, p123 = (p12 + p23) * 0.5f;
            const Vec2 mid = (p012 + p123) * 0.5f;
            FlattenCubic(out, p0, p01, p012, mid, tolerance, depth + 1);
            FlattenCubic(out, mid, p123, p23, p3, tolerance, depth + 1);
        }

    }

    std::vector<Contour> Path::Flatten(const Affine& transform, f32 tolerance) const {
        std::vector<Contour> contours;
        Contour* current = nullptr;
        Vec2 pen{ 0.0f, 0.0f };
        std::size_t at = 0;

        const f32 error = std::max(tolerance, 1e-3f);

        const auto begin = [&](Vec2 p) {
            contours.push_back({});
            current = &contours.back();
            current->closed = false;
            current->points.push_back(p);
            pen = p;
        };

        for (const Verb verb : m_Verbs) {
            switch (verb) {
                case Verb::Move:
                    begin(transform.Apply(m_Points[at]));
                    at += 1;
                    break;
                case Verb::Line: {
                    const Vec2 p = transform.Apply(m_Points[at]);
                    at += 1;
                    if (!current) { begin(p); break; }
                    current->points.push_back(p);
                    pen = p;
                    break;
                }
                case Verb::Quad: {
                    const Vec2 control = transform.Apply(m_Points[at]);
                    const Vec2 to = transform.Apply(m_Points[at + 1]);
                    at += 2;
                    if (!current) { begin(to); break; }
                    // A quadratic is a cubic whose handles sit two thirds of the way along, so
                    // there is one subdivision routine rather than two that must agree.
                    FlattenCubic(current->points, pen, pen + (control - pen) * (2.0f / 3.0f),
                                 to + (control - to) * (2.0f / 3.0f), to, error, 0);
                    pen = to;
                    break;
                }
                case Verb::Cubic: {
                    const Vec2 c1 = transform.Apply(m_Points[at]);
                    const Vec2 c2 = transform.Apply(m_Points[at + 1]);
                    const Vec2 to = transform.Apply(m_Points[at + 2]);
                    at += 3;
                    if (!current) { begin(to); break; }
                    FlattenCubic(current->points, pen, c1, c2, to, error, 0);
                    pen = to;
                    break;
                }
                case Verb::Close:
                    if (current) {
                        current->closed = true;
                        pen = current->points.front();
                    }
                    break;
            }
        }

        std::erase_if(contours, [](const Contour& c) { return c.points.size() < 2; });
        return contours;
    }

    Rect Path::ControlBounds() const {
        if (m_Points.empty()) return {};
        Vec2 lo = m_Points.front(), hi = m_Points.front();
        for (const Vec2 p : m_Points) {
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y) };
        }
        return Rect::FromEdges(lo.x, lo.y, hi.x, hi.y);
    }

}
