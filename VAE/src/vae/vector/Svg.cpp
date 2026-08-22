#include "vaepch.h"
#include "vae/vector/Svg.h"

#include <charconv>
#include <cmath>
#include <map>

namespace vae::vector {

    namespace {

        bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

        void SkipSpace(std::string_view s, std::size_t& at) {
            while (at < s.size() && IsSpace(s[at])) ++at;
        }

        std::string_view Trim(std::string_view s) {
            std::size_t begin = 0, end = s.size();
            while (begin < end && IsSpace(s[begin])) ++begin;
            while (end > begin && IsSpace(s[end - 1])) --end;
            return s.substr(begin, end - begin);
        }

        // A number as SVG spells it, which is not quite what strtof accepts: "1.5.5" is two numbers
        // and "1-2" is two numbers, because the grammar lets a sign start one without a separator.
        bool ReadNumber(std::string_view s, std::size_t& at, f32& out) {
            while (at < s.size() && (IsSpace(s[at]) || s[at] == ',')) ++at;
            const std::size_t begin = at;
            if (at < s.size() && (s[at] == '+' || s[at] == '-')) ++at;
            bool dot = false, digits = false;
            while (at < s.size()) {
                const char c = s[at];
                if (c >= '0' && c <= '9') { digits = true; ++at; continue; }
                if (c == '.' && !dot) { dot = true; ++at; continue; }
                break;
            }
            if (!digits) { at = begin; return false; }
            if (at < s.size() && (s[at] == 'e' || s[at] == 'E')) {
                const std::size_t mark = at;
                ++at;
                if (at < s.size() && (s[at] == '+' || s[at] == '-')) ++at;
                if (at < s.size() && s[at] >= '0' && s[at] <= '9') {
                    while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
                } else {
                    at = mark;
                }
            }
            out = std::strtof(std::string(s.substr(begin, at - begin)).c_str(), nullptr);
            return true;
        }

        // A length with a unit suffix. Everything is treated as user units except percentages,
        // which have no meaning without a viewport and are refused rather than guessed at.
        f32 ReadLength(std::string_view text, f32 fallback) {
            std::size_t at = 0;
            f32 value = 0.0f;
            if (!ReadNumber(text, at, value)) return fallback;
            const std::string_view unit = Trim(text.substr(at));
            if (unit.empty() || unit == "px") return value;
            if (unit == "pt") return value * (96.0f / 72.0f);
            if (unit == "pc") return value * 16.0f;
            if (unit == "mm") return value * (96.0f / 25.4f);
            if (unit == "cm") return value * (96.0f / 2.54f);
            if (unit == "in") return value * 96.0f;
            return value;
        }

        // ------------------------------------------------------------------------------ colour

        const std::map<std::string_view, u32>& NamedColours() {
            // The names an icon set or an exporter actually emits. The full CSS list is 148 entries
            // of which this is the part anyone has ever used in an SVG.
            static const std::map<std::string_view, u32> table{
                { "black", 0x000000 }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 },
                { "grey", 0x808080 }, { "white", 0xFFFFFF }, { "maroon", 0x800000 },
                { "red", 0xFF0000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
                { "green", 0x008000 }, { "lime", 0x00FF00 }, { "olive", 0x808000 },
                { "yellow", 0xFFFF00 }, { "navy", 0x000080 }, { "blue", 0x0000FF },
                { "teal", 0x008080 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF },
                { "magenta", 0xFF00FF }, { "orange", 0xFFA500 }, { "pink", 0xFFC0CB },
                { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "indigo", 0x4B0082 },
                { "violet", 0xEE82EE }, { "transparent", 0x000000 },
            };
            return table;
        }

        int HexDigit(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        enum class PaintKind { None, Colour, CurrentColor, Unsupported };

        PaintKind ReadPaint(std::string_view text, Color& out) {
            text = Trim(text);
            if (text.empty() || text == "none") return PaintKind::None;
            if (text == "currentColor") return PaintKind::CurrentColor;
            if (text.starts_with("url(")) return PaintKind::Unsupported;   // a gradient or pattern

            if (text[0] == '#') {
                const std::string_view digits = text.substr(1);
                const auto channel = [&](int i) { return static_cast<f32>(HexDigit(digits[i])); };
                if (digits.size() >= 6) {
                    out = { (channel(0) * 16 + channel(1)) / 255.0f,
                            (channel(2) * 16 + channel(3)) / 255.0f,
                            (channel(4) * 16 + channel(5)) / 255.0f,
                            digits.size() >= 8 ? (channel(6) * 16 + channel(7)) / 255.0f : 1.0f };
                    return PaintKind::Colour;
                }
                if (digits.size() >= 3) {
                    out = { channel(0) / 15.0f, channel(1) / 15.0f, channel(2) / 15.0f,
                            digits.size() >= 4 ? channel(3) / 15.0f : 1.0f };
                    return PaintKind::Colour;
                }
                return PaintKind::Unsupported;
            }

            if (text.starts_with("rgb")) {
                const std::size_t open = text.find('(');
                if (open == std::string_view::npos) return PaintKind::Unsupported;
                std::size_t at = open + 1;
                f32 values[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
                int count = 0;
                while (count < 4 && ReadNumber(text, at, values[count])) {
                    if (at < text.size() && text[at] == '%') { values[count] *= 2.55f; ++at; }
                    ++count;
                }
                if (count < 3) return PaintKind::Unsupported;
                out = { values[0] / 255.0f, values[1] / 255.0f, values[2] / 255.0f,
                        count > 3 ? values[3] : 1.0f };
                return PaintKind::Colour;
            }

            if (const auto it = NamedColours().find(text); it != NamedColours().end()) {
                const u32 rgb = it->second;
                out = { static_cast<f32>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<f32>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<f32>(rgb & 0xFF) / 255.0f,
                        text == "transparent" ? 0.0f : 1.0f };
                return PaintKind::Colour;
            }
            return PaintKind::Unsupported;
        }

        // --------------------------------------------------------------------------- transforms

        Affine ReadTransform(std::string_view text) {
            Affine result;
            std::size_t at = 0;
            while (at < text.size()) {
                SkipSpace(text, at);
                const std::size_t nameStart = at;
                while (at < text.size() && (std::isalpha(static_cast<unsigned char>(text[at]))))
                    ++at;
                const std::string_view name = text.substr(nameStart, at - nameStart);
                SkipSpace(text, at);
                if (at >= text.size() || text[at] != '(') break;
                ++at;

                f32 values[6]{};
                int count = 0;
                while (count < 6 && ReadNumber(text, at, values[count])) ++count;
                SkipSpace(text, at);
                if (at < text.size() && text[at] == ')') ++at;
                while (at < text.size() && (IsSpace(text[at]) || text[at] == ',')) ++at;

                Affine step;
                if (name == "matrix" && count >= 6)
                    step = { values[0], values[1], values[2], values[3], values[4], values[5] };
                else if (name == "translate" && count >= 1)
                    step = Affine::Translate({ values[0], count > 1 ? values[1] : 0.0f });
                else if (name == "scale" && count >= 1)
                    step = Affine::Scaling({ values[0], count > 1 ? values[1] : values[0] });
                else if (name == "rotate" && count >= 1) {
                    step = Affine::Rotate(values[0]);
                    if (count >= 3) {
                        const Vec2 pivot{ values[1], values[2] };
                        step = Affine::Translate(-pivot).Then(step).Then(Affine::Translate(pivot));
                    }
                } else if (name == "skewX" && count >= 1) step = Affine::SkewX(values[0]);
                else if (name == "skewY" && count >= 1)   step = Affine::SkewY(values[0]);
                else if (name.empty()) break;

                // Left to right in the attribute means outermost last, the same as nesting.
                result = step.Then(result);
            }
            return result;
        }

        // -------------------------------------------------------------------------- path data

        void ReadPathData(std::string_view d, Path& path) {
            std::size_t at = 0;
            char command = 0;
            const auto relative = [&] { return command >= 'a' && command <= 'z'; };
            const auto offset = [&] { return relative() ? path.Current() : Vec2{ 0.0f, 0.0f }; };

            const auto number = [&](f32& value) { return ReadNumber(d, at, value); };
            const auto point = [&](Vec2& value) {
                return number(value.x) && number(value.y);
            };

            while (at < d.size()) {
                SkipSpace(d, at);
                if (at >= d.size()) break;
                if (std::isalpha(static_cast<unsigned char>(d[at]))) { command = d[at]; ++at; }
                else if (command == 0) break;
                // A repeated coordinate set continues the previous command — except after a
                // moveto, where the specification says it becomes a lineto.
                else if (command == 'M') command = 'L';
                else if (command == 'm') command = 'l';

                const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
                Vec2 a{}, b{}, c{};
                switch (upper) {
                    case 'M': if (point(a)) path.MoveTo(a + offset()); break;
                    case 'L': if (point(a)) path.LineTo(a + offset()); break;
                    case 'H': if (number(a.x))
                                  path.LineTo({ a.x + offset().x, path.Current().y }); break;
                    case 'V': if (number(a.y))
                                  path.LineTo({ path.Current().x, a.y + offset().y }); break;
                    case 'C': {
                        const Vec2 base = offset();
                        if (point(a) && point(b) && point(c))
                            path.CubicTo(a + base, b + base, c + base);
                        break;
                    }
                    case 'S': {
                        const Vec2 base = offset();
                        const Vec2 mirrored = path.ReflectedControl(true);
                        if (point(b) && point(c)) path.CubicTo(mirrored, b + base, c + base);
                        break;
                    }
                    case 'Q': {
                        const Vec2 base = offset();
                        if (point(a) && point(b)) path.QuadTo(a + base, b + base);
                        break;
                    }
                    case 'T': {
                        const Vec2 base = offset();
                        const Vec2 mirrored = path.ReflectedControl(false);
                        if (point(b)) path.QuadTo(mirrored, b + base);
                        break;
                    }
                    case 'A': {
                        const Vec2 base = offset();
                        Vec2 radii{};
                        f32 rotation = 0.0f, largeArc = 0.0f, sweep = 0.0f;
                        if (point(radii) && number(rotation) && number(largeArc) && number(sweep)
                            && point(c))
                            path.ArcTo(radii, rotation, largeArc != 0.0f, sweep != 0.0f, c + base);
                        break;
                    }
                    case 'Z': path.Close(); break;
                    default: return;      // an unknown letter: stop rather than misread the rest
                }
                if (upper == 'Z') SkipSpace(d, at);
            }
        }

        // ------------------------------------------------------------------------ the XML half

        struct Attributes {
            std::map<std::string, std::string> values;

            std::string_view Get(std::string_view name, std::string_view fallback = {}) const {
                const auto it = values.find(std::string(name));
                return it == values.end() ? fallback : std::string_view(it->second);
            }
            bool Has(std::string_view name) const {
                return values.contains(std::string(name));
            }
            f32 Number(std::string_view name, f32 fallback) const {
                const auto it = values.find(std::string(name));
                return it == values.end() ? fallback : ReadLength(it->second, fallback);
            }
        };

        // Presentation attributes and `style` say the same things, so they are read into the same
        // place: whichever comes later in the element wins, which is what a browser does too.
        void ReadStyle(std::string_view style, Attributes& into) {
            std::size_t at = 0;
            while (at < style.size()) {
                const std::size_t colon = style.find(':', at);
                if (colon == std::string_view::npos) break;
                std::size_t end = style.find(';', colon);
                if (end == std::string_view::npos) end = style.size();
                into.values[std::string(Trim(style.substr(at, colon - at)))] =
                    std::string(Trim(style.substr(colon + 1, end - colon - 1)));
                at = end + 1;
            }
        }

        struct Element {
            std::string name;
            Attributes attributes;
            bool closing = false;
            bool selfClosing = false;
        };

        // Enough XML for a picture: elements, attributes, comments, processing instructions and
        // doctypes. No entities beyond the five, no namespaces beyond ignoring the prefix — an SVG
        // that needs more than this needs a real parser, and says so rather than half-loading.
        bool NextElement(std::string_view source, std::size_t& at, Element& out) {
            while (at < source.size()) {
                const std::size_t open = source.find('<', at);
                if (open == std::string_view::npos) return false;
                at = open + 1;
                if (source.compare(at, 3, "!--") == 0) {
                    const std::size_t end = source.find("-->", at);
                    at = end == std::string_view::npos ? source.size() : end + 3;
                    continue;
                }
                if (at < source.size() && (source[at] == '?' || source[at] == '!')) {
                    const std::size_t end = source.find('>', at);
                    at = end == std::string_view::npos ? source.size() : end + 1;
                    continue;
                }

                out = {};
                if (at < source.size() && source[at] == '/') { out.closing = true; ++at; }

                const std::size_t nameStart = at;
                while (at < source.size() && !IsSpace(source[at]) && source[at] != '>'
                       && source[at] != '/')
                    ++at;
                std::string_view name = source.substr(nameStart, at - nameStart);
                if (const std::size_t colon = name.rfind(':'); colon != std::string_view::npos)
                    name = name.substr(colon + 1);
                out.name = std::string(name);

                while (at < source.size()) {
                    SkipSpace(source, at);
                    if (at >= source.size() || source[at] == '>') break;
                    if (source[at] == '/') { out.selfClosing = true; ++at; continue; }

                    const std::size_t keyStart = at;
                    while (at < source.size() && source[at] != '=' && !IsSpace(source[at])
                           && source[at] != '>')
                        ++at;
                    std::string_view key = source.substr(keyStart, at - keyStart);
                    if (const std::size_t colon = key.rfind(':'); colon != std::string_view::npos)
                        key = key.substr(colon + 1);
                    SkipSpace(source, at);
                    if (at >= source.size() || source[at] != '=') continue;
                    ++at;
                    SkipSpace(source, at);
                    if (at >= source.size()) break;
                    const char quote = source[at];
                    if (quote != '"' && quote != '\'') continue;
                    ++at;
                    const std::size_t valueStart = at;
                    while (at < source.size() && source[at] != quote) ++at;
                    std::string value(source.substr(valueStart, at - valueStart));
                    if (at < source.size()) ++at;

                    // The five predefined entities, which is all an SVG's attribute values carry.
                    for (const auto& [entity, character] :
                         { std::pair<std::string_view, char>{ "&amp;", '&' },
                           { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } })
                        for (std::size_t found = value.find(entity); found != std::string::npos;
                             found = value.find(entity, found + 1))
                            value.replace(found, entity.size(), 1, character);

                    if (key == "style") ReadStyle(value, out.attributes);
                    else out.attributes.values[std::string(key)] = std::move(value);
                }
                if (at < source.size() && source[at] == '>') ++at;
                return true;
            }
            return false;
        }

        // The paint state an element inherits from everything around it.
        struct Inherited {
            Affine transform;
            bool hasFill = true;
            Color fill{ 0.0f, 0.0f, 0.0f, 1.0f };
            bool fillFollowsText = false;
            bool hasStroke = false;
            Color stroke{ 0.0f, 0.0f, 0.0f, 1.0f };
            bool strokeFollowsText = false;
            f32 strokeWidth = 1.0f;
            LineCap cap = LineCap::Butt;
            LineJoin join = LineJoin::Miter;
            f32 miterLimit = 4.0f;
            FillRule rule = FillRule::NonZero;
            f32 opacity = 1.0f;
        };

        void Inherit(Inherited& state, const Attributes& attributes, std::string* dropped) {
            if (attributes.Has("transform"))
                state.transform = ReadTransform(attributes.Get("transform")).Then(state.transform);

            if (attributes.Has("fill")) {
                Color colour{};
                switch (ReadPaint(attributes.Get("fill"), colour)) {
                    case PaintKind::None:  state.hasFill = false; state.fillFollowsText = false; break;
                    case PaintKind::Colour: state.hasFill = true; state.fill = colour;
                                            state.fillFollowsText = false; break;
                    case PaintKind::CurrentColor: state.hasFill = true;
                                                  state.fillFollowsText = true; break;
                    case PaintKind::Unsupported:
                        state.hasFill = true;
                        state.fillFollowsText = true;
                        if (dropped && dropped->empty())
                            *dropped = "gradients and patterns are not read yet — "
                                       "those shapes take the tint instead";
                        break;
                }
            }
            if (attributes.Has("stroke")) {
                Color colour{};
                switch (ReadPaint(attributes.Get("stroke"), colour)) {
                    case PaintKind::None: state.hasStroke = false; state.strokeFollowsText = false; break;
                    case PaintKind::Colour: state.hasStroke = true; state.stroke = colour;
                                            state.strokeFollowsText = false; break;
                    default: state.hasStroke = true; state.strokeFollowsText = true; break;
                }
            }
            if (attributes.Has("stroke-width"))
                state.strokeWidth = attributes.Number("stroke-width", state.strokeWidth);
            if (attributes.Has("stroke-linecap")) {
                const std::string_view value = attributes.Get("stroke-linecap");
                state.cap = value == "round" ? LineCap::Round
                          : value == "square" ? LineCap::Square : LineCap::Butt;
            }
            if (attributes.Has("stroke-linejoin")) {
                const std::string_view value = attributes.Get("stroke-linejoin");
                state.join = value == "round" ? LineJoin::Round
                           : value == "bevel" ? LineJoin::Bevel : LineJoin::Miter;
            }
            if (attributes.Has("stroke-miterlimit"))
                state.miterLimit = attributes.Number("stroke-miterlimit", state.miterLimit);
            if (attributes.Has("fill-rule"))
                state.rule = attributes.Get("fill-rule") == "evenodd" ? FillRule::EvenOdd
                                                                      : FillRule::NonZero;
            if (attributes.Has("opacity"))
                state.opacity *= attributes.Number("opacity", 1.0f);
            // fill-opacity and stroke-opacity fold into the colours they belong to, so the shape
            // does not need to carry two more numbers nobody downstream would remember to apply.
            if (attributes.Has("fill-opacity")) state.fill.a *= attributes.Number("fill-opacity", 1.0f);
            if (attributes.Has("stroke-opacity"))
                state.stroke.a *= attributes.Number("stroke-opacity", 1.0f);
        }

        void RoundedRect(Path& path, Rect box, f32 rx, f32 ry) {
            rx = std::min(rx, box.size.x * 0.5f);
            ry = std::min(ry, box.size.y * 0.5f);
            if (rx <= 0.0f || ry <= 0.0f) {
                path.MoveTo(box.pos);
                path.LineTo({ box.Right(), box.Top() });
                path.LineTo({ box.Right(), box.Bottom() });
                path.LineTo({ box.Left(), box.Bottom() });
                path.Close();
                return;
            }
            path.MoveTo({ box.Left() + rx, box.Top() });
            path.LineTo({ box.Right() - rx, box.Top() });
            path.ArcTo({ rx, ry }, 0.0f, false, true, { box.Right(), box.Top() + ry });
            path.LineTo({ box.Right(), box.Bottom() - ry });
            path.ArcTo({ rx, ry }, 0.0f, false, true, { box.Right() - rx, box.Bottom() });
            path.LineTo({ box.Left() + rx, box.Bottom() });
            path.ArcTo({ rx, ry }, 0.0f, false, true, { box.Left(), box.Bottom() - ry });
            path.LineTo({ box.Left(), box.Top() + ry });
            path.ArcTo({ rx, ry }, 0.0f, false, true, { box.Left() + rx, box.Top() });
            path.Close();
        }

        void Ellipse(Path& path, Vec2 centre, Vec2 radii) {
            path.MoveTo({ centre.x + radii.x, centre.y });
            path.ArcTo(radii, 0.0f, false, true, { centre.x - radii.x, centre.y });
            path.ArcTo(radii, 0.0f, false, true, { centre.x + radii.x, centre.y });
            path.Close();
        }

        void Points(Path& path, std::string_view text, bool close) {
            std::size_t at = 0;
            Vec2 p{};
            bool first = true;
            while (ReadNumber(text, at, p.x) && ReadNumber(text, at, p.y)) {
                if (first) { path.MoveTo(p); first = false; }
                else path.LineTo(p);
            }
            if (close && !first) path.Close();
        }

    }

    bool ParseSvg(std::string_view source, Picture& out, std::string* error) {
        out = {};
        std::string dropped;

        std::size_t at = 0;
        Element element;
        std::vector<Inherited> stack{ Inherited{} };
        // `<defs>` and friends define things for other elements to reference; drawing them where
        // they sit is the single most visible way to get an SVG wrong.
        int hidden = 0;
        bool sawSvg = false;

        while (NextElement(source, at, element)) {
            const std::string& name = element.name;

            if (element.closing) {
                if (name == "defs" || name == "clipPath" || name == "mask" || name == "symbol"
                    || name == "pattern" || name == "marker") {
                    if (hidden > 0) --hidden;
                } else if (name == "g" || name == "svg" || name == "a") {
                    if (stack.size() > 1) stack.pop_back();
                }
                continue;
            }

            if (name == "defs" || name == "clipPath" || name == "mask" || name == "symbol"
                || name == "pattern" || name == "marker") {
                if (!element.selfClosing) ++hidden;
                continue;
            }
            if (hidden > 0) continue;

            if (name == "svg") {
                sawSvg = true;
                const std::string_view box = element.attributes.Get("viewBox");
                if (!box.empty()) {
                    std::size_t cursor = 0;
                    f32 values[4]{};
                    int count = 0;
                    while (count < 4 && ReadNumber(box, cursor, values[count])) ++count;
                    if (count == 4)
                        out.viewBox = Rect{ { values[0], values[1] }, { values[2], values[3] } };
                }
                out.size = { element.attributes.Number("width", out.viewBox.size.x),
                             element.attributes.Number("height", out.viewBox.size.y) };
                if (out.viewBox.size.x <= 0.0f || out.viewBox.size.y <= 0.0f)
                    out.viewBox = Rect{ { 0.0f, 0.0f }, out.size };
                if (out.size.x <= 0.0f || out.size.y <= 0.0f) out.size = out.viewBox.size;

                Inherited state = stack.back();
                Inherit(state, element.attributes, &dropped);
                if (element.selfClosing) continue;
                stack.push_back(state);
                continue;
            }

            if (name == "g" || name == "a") {
                Inherited state = stack.back();
                Inherit(state, element.attributes, &dropped);
                if (!element.selfClosing) stack.push_back(state);
                continue;
            }

            Path path;
            if (name == "path") ReadPathData(element.attributes.Get("d"), path);
            else if (name == "rect") {
                const Rect box{ { element.attributes.Number("x", 0.0f),
                                  element.attributes.Number("y", 0.0f) },
                                { element.attributes.Number("width", 0.0f),
                                  element.attributes.Number("height", 0.0f) } };
                if (box.size.x <= 0.0f || box.size.y <= 0.0f) continue;
                // One radius given means both, which is what "rounded corners" almost always means.
                const f32 rx = element.attributes.Number("rx",
                                   element.attributes.Number("ry", 0.0f));
                const f32 ry = element.attributes.Number("ry", rx);
                RoundedRect(path, box, rx, ry);
            } else if (name == "circle") {
                const f32 r = element.attributes.Number("r", 0.0f);
                if (r <= 0.0f) continue;
                Ellipse(path, { element.attributes.Number("cx", 0.0f),
                                element.attributes.Number("cy", 0.0f) }, { r, r });
            } else if (name == "ellipse") {
                const Vec2 radii{ element.attributes.Number("rx", 0.0f),
                                  element.attributes.Number("ry", 0.0f) };
                if (radii.x <= 0.0f || radii.y <= 0.0f) continue;
                Ellipse(path, { element.attributes.Number("cx", 0.0f),
                                element.attributes.Number("cy", 0.0f) }, radii);
            } else if (name == "line") {
                path.MoveTo({ element.attributes.Number("x1", 0.0f),
                              element.attributes.Number("y1", 0.0f) });
                path.LineTo({ element.attributes.Number("x2", 0.0f),
                              element.attributes.Number("y2", 0.0f) });
            } else if (name == "polyline" || name == "polygon") {
                Points(path, element.attributes.Get("points"), name == "polygon");
            } else {
                if ((name == "text" || name == "image" || name == "use" || name == "foreignObject")
                    && dropped.empty())
                    dropped = "<" + name + "> is not read yet and was left out";
                continue;
            }

            if (path.Empty()) continue;

            Inherited state = stack.back();
            Inherit(state, element.attributes, &dropped);

            Shape shape;
            shape.path = std::move(path);
            shape.transform = state.transform;
            shape.hasFill = state.hasFill;
            shape.fill = state.fill;
            shape.fillFollowsText = state.fillFollowsText;
            shape.rule = state.rule;
            shape.hasStroke = state.hasStroke && state.strokeWidth > 0.0f;
            shape.stroke = state.stroke;
            shape.strokeFollowsText = state.strokeFollowsText;
            shape.strokeWidth = state.strokeWidth;
            shape.cap = state.cap;
            shape.join = state.join;
            shape.miterLimit = state.miterLimit;
            shape.opacity = state.opacity;
            // A `line` or `polyline` with no stroke draws nothing at all; a `path` with neither
            // fill nor stroke stated is filled black, which is SVG's default and not a mistake.
            if ((name == "line" || name == "polyline") && !shape.hasStroke) continue;
            if (!shape.hasFill && !shape.hasStroke) continue;
            if (name == "line" || name == "polyline") shape.hasFill = false;

            out.shapes.push_back(std::move(shape));
        }

        if (!sawSvg) {
            if (error) *error = "not an SVG: no <svg> element";
            return false;
        }
        if (out.shapes.empty()) {
            if (error) *error = dropped.empty() ? "the SVG has nothing drawable in it" : dropped;
            return false;
        }
        if (error) *error = dropped;
        return true;
    }

    // ------------------------------------------------------------------------------- rendering

    Bitmap Render(const Picture& picture, u32 width, u32 height, const Color* tint) {
        Bitmap bitmap;
        if (width == 0 || height == 0 || picture.Empty()) return bitmap;

        bitmap.width = width;
        bitmap.height = height;
        bitmap.pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);

        const Rect box = picture.viewBox.size.x > 0.0f && picture.viewBox.size.y > 0.0f
                       ? picture.viewBox
                       : Rect{ { 0.0f, 0.0f }, picture.size };
        if (box.size.x <= 0.0f || box.size.y <= 0.0f) return bitmap;

        // Fitted, not stretched, and centred in whatever is left over: an icon squashed to fill a
        // box is an icon nobody recognises.
        const f32 scale = std::min(static_cast<f32>(width) / box.size.x,
                                   static_cast<f32>(height) / box.size.y);
        const Vec2 offset{ (static_cast<f32>(width) - box.size.x * scale) * 0.5f,
                           (static_cast<f32>(height) - box.size.y * scale) * 0.5f };
        const Affine toPixels = Affine::Translate(-box.pos)
                                    .Then(Affine::Scaling({ scale, scale }))
                                    .Then(Affine::Translate(offset));

        // Straight alpha, composited "over" one layer at a time, because that is what the shader
        // will sample and what every other texture in the engine holds.
        const auto blend = [&](const Mask& mask, Color colour) {
            if (mask.Empty() || colour.a <= 0.0f) return;
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const f32 coverage = static_cast<f32>(mask.At(x, y)) / 255.0f * colour.a;
                    if (coverage <= 0.0f) continue;
                    u8* pixel = bitmap.pixels.data()
                              + (static_cast<std::size_t>(y) * width + x) * 4;
                    const f32 dstA = static_cast<f32>(pixel[3]) / 255.0f;
                    const f32 outA = coverage + dstA * (1.0f - coverage);
                    for (int channel = 0; channel < 3; ++channel) {
                        const f32 dst = static_cast<f32>(pixel[channel]) / 255.0f;
                        const f32 src = channel == 0 ? colour.r : channel == 1 ? colour.g : colour.b;
                        const f32 value = outA <= 1e-6f ? 0.0f
                                        : (src * coverage + dst * dstA * (1.0f - coverage)) / outA;
                        pixel[channel] = static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    pixel[3] = static_cast<u8>(std::clamp(outA, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        };

        // What a tint replaces. A file that says `currentColor` has named the parts that are
        // asking to be told, and only those change — that is what keeps a two-tone icon two-tone.
        // A file that never says it has no such parts, so the tint means the whole thing: an icon
        // that hardcoded black still has to be able to obey a theme.
        const bool selective = picture.FollowsText();

        for (const Shape& shape : picture.shapes) {
            const Affine transform = shape.transform.Then(toPixels);
            // Half a pixel of error: finer is invisible and coarser shows on a curve's shoulder.
            const std::vector<Contour> contours = shape.path.Flatten(transform, 0.25f);
            if (contours.empty()) continue;

            if (shape.hasFill) {
                const bool recolour = tint && (!selective || shape.fillFollowsText);
                Color colour = recolour ? *tint : shape.fill;
                colour.a *= shape.opacity;
                blend(Fill(contours, shape.rule, width, height), colour);
            }
            if (shape.hasStroke) {
                const bool recolour = tint && (!selective || shape.strokeFollowsText);
                Color colour = recolour ? *tint : shape.stroke;
                colour.a *= shape.opacity;
                const f32 strokeWidth = std::max(shape.strokeWidth * transform.Scale(), 0.75f);
                blend(Fill(Stroke(contours, strokeWidth, shape.join, shape.cap, shape.miterLimit),
                           FillRule::NonZero, width, height),
                      colour);
            }
        }

        return bitmap;
    }

}
