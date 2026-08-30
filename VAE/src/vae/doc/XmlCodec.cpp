#include "vaepch.h"
#include "vae/doc/Serializer.h"

#include "vae/doc/ValueText.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_set>

// Format 3: the document as markup.
//
// The element name is the node kind and the tree is the indentation, which is the whole readability
// win in two decisions. Everything else follows the rule format 2 already established — write only
// what the reader cannot work out — pushed as far as an attribute-per-property format allows:
// layout fields equal to their default are absent, enums are names, a size is "72" or "fill" rather
// than a {mode, value} pair, and a property's declared type means a string does not have to be
// quoted into a corner.
//
// design/xml-format.md is the specification; this file is its implementation.
namespace vae::doc {

    using namespace vae::doc::text;

    namespace {

        // ------------------------------------------------------------------ names

        const char* LayoutModeName(layout::LayoutMode m) {
            switch (m) {
                case layout::LayoutMode::Absolute: return "absolute";
                case layout::LayoutMode::Stack:    return "stack";
                case layout::LayoutMode::Grid:     return "grid";
            }
            return "absolute";
        }

        const char* AxisName(layout::Axis a) {
            return a == layout::Axis::Row ? "row" : "column";
        }

        const char* AlignName(layout::Align a) {
            switch (a) {
                case layout::Align::Start:   return "start";
                case layout::Align::Center:  return "center";
                case layout::Align::End:     return "end";
                case layout::Align::Stretch: return "stretch";
            }
            return "start";
        }

        const char* JustifyName(layout::Justify j) {
            switch (j) {
                case layout::Justify::Start:        return "start";
                case layout::Justify::Center:       return "center";
                case layout::Justify::End:          return "end";
                case layout::Justify::SpaceBetween: return "spaceBetween";
                case layout::Justify::SpaceAround:  return "spaceAround";
                case layout::Justify::SpaceEvenly:  return "spaceEvenly";
            }
            return "start";
        }

        const char* ConstraintName(layout::Constraint c) {
            switch (c) {
                case layout::Constraint::Start:    return "start";
                case layout::Constraint::End:      return "end";
                case layout::Constraint::StartEnd: return "startEnd";
                case layout::Constraint::Center:   return "center";
                case layout::Constraint::Scale:    return "scale";
            }
            return "start";
        }

        // ------------------------------------------------------------------ scalars

        // A size says its mode in its spelling: a bare number is pixels, which is the case that
        // appears on 39 of Vaecord's 61 nodes and the one worth making shortest.
        std::string SizeText(const layout::Size& size) {
            switch (size.mode) {
                case layout::SizeMode::Fixed:   return Number(size.value);
                case layout::SizeMode::Hug:     return "hug";
                case layout::SizeMode::Fill:    return size.value == 1.0f ? "fill"
                                                                         : "fill " + Number(size.value);
                case layout::SizeMode::Percent: return Number(size.value * 100.0f) + "%";
            }
            return "hug";
        }

        // One value for all four, two for horizontal and vertical, four for l t r b. The short
        // forms are exactly Edges(f32) and Edges(h, v), so the file and the constructor agree —
        // note this is NOT css order, which is t r b l.
        std::string EdgesText(const Edges& e) {
            if (e.left == e.right && e.top == e.bottom)
                return e.left == e.top ? Number(e.left) : Number(e.left) + " " + Number(e.top);
            return Number(e.left) + " " + Number(e.top) + " " + Number(e.right) + " "
                 + Number(e.bottom);
        }

        std::string ColorText(const Color& c) {
            if (auto hex = ColorToHex(c)) return *hex;
            return Number(c.r) + " " + Number(c.g) + " " + Number(c.b) + " " + Number(c.a);
        }

        // Whitespace-separated numbers, or nothing. This is what makes a colour and a vector
        // self-describing: nothing else in the format is four numbers or two.
        std::optional<std::vector<f32>> Numbers(std::string_view s) {
            std::vector<f32> out;
            std::size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && s[i] == ' ') ++i;
                if (i >= s.size()) break;
                const std::size_t start = i;
                while (i < s.size() && s[i] != ' ') ++i;
                auto n = ParseNumber(s.substr(start, i - start));
                if (!n) return std::nullopt;
                out.push_back(*n);
            }
            if (out.empty()) return std::nullopt;
            return out;
        }

        // The single place that decides what an attribute means. `declared` is what the property
        // says it holds (ValueType::Unset for a custom property, which has nobody to declare it);
        // sigils are checked first and so survive any declaration, which is what lets `fill` hold
        // "@accent" even though fill is declared a colour.
        Value ValueFromAttr(std::string_view s, ValueType declared) {
            if (!s.empty()) {
                switch (s[0]) {
                    case '$': return std::string(s.substr(1));
                    case '@': return TokenRef{ std::string(s.substr(1)) };
                    case '=': return Binding{ std::string(s.substr(1)) };
                    case '&': return Uuid::FromString(std::string(s.substr(1)));
                    case '*': return AssetRef{ Uuid::FromString(std::string(s.substr(1))) };
                    case '#': if (auto c = ColorFromHex(s)) return *c;
                              return std::string(s);        // not a colour after all: a literal
                    default:  break;
                }
            }
            // A property declared to hold text holds text, full stop. Without this rule a label
            // reading "1" comes back a number and one reading "12 8" comes back a vector.
            if (declared == ValueType::Text) return std::string(s);

            if (auto nums = Numbers(s)) {
                if (nums->size() == 4) return Color{ (*nums)[0], (*nums)[1], (*nums)[2], (*nums)[3] };
                if (nums->size() == 2) return Vec2{ (*nums)[0], (*nums)[1] };
                if (nums->size() == 1) return (*nums)[0];
            }
            if (s == "true")  return true;
            if (s == "false") return false;
            return std::string(s);
        }

        // The attribute text for a value, or nothing when no attribute could carry it. Verified by
        // reading it straight back: if `ValueFromAttr` would not return what went in, the property
        // goes out through the explicit <prop> element instead. That makes round-tripping a
        // property of the encoder rather than something to hope for.
        std::optional<std::string> ValueToAttr(const Value& value, ValueType declared) {
            std::string out;
            bool encodable = true;
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>)      encodable = false;
                else if constexpr (std::is_same_v<T, bool>)           out = v ? "true" : "false";
                else if constexpr (std::is_same_v<T, f32>)            out = Number(v);
                else if constexpr (std::is_same_v<T, Vec2>)           out = Vec2Text(v);
                else if constexpr (std::is_same_v<T, Color>)          out = ColorText(v);
                else if constexpr (std::is_same_v<T, std::string>)    out = EscapeLiteral(v);
                else if constexpr (std::is_same_v<T, Uuid>)           out = "&" + v.ToString();
                else if constexpr (std::is_same_v<T, AssetRef>)       out = "*" + v.id.ToString();
                else if constexpr (std::is_same_v<T, TokenRef>)       out = "@" + v.name;
                else if constexpr (std::is_same_v<T, Binding>)        out = "=" + v.expression;
            }, value);
            if (!encodable) return std::nullopt;

            if (ValueFromAttr(out, declared) == value) return out;

            // A string that would read back as something else: "1" on a property that is not
            // declared text. The '$' escape is exactly for this, and it is the common case.
            if (std::holds_alternative<std::string>(value)) {
                const std::string escaped = "$" + std::get<std::string>(value);
                if (ValueFromAttr(escaped, declared) == value) return escaped;
            }
            return std::nullopt;
        }

        // ------------------------------------------------------------------ writing

        std::string EscapeBody(std::string_view s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                switch (c) {
                    case '&': out += "&amp;"; break;
                    case '<': out += "&lt;";  break;
                    case '>': out += "&gt;";  break;
                    default:  out += c;       break;
                }
            }
            return out;
        }

        // An attribute name has to be a name. Prop names are camelCase and state overlays are
        // rewritten to `hovered.fill` below, so this only ever rejects something a project invented
        // for itself — which then goes out as a <prop> element rather than being mangled.
        bool IsXmlName(std::string_view s) {
            if (s.empty()) return false;
            const auto nameStart = [](unsigned char c) {
                return std::isalpha(c) || c == '_';
            };
            const auto nameChar = [](unsigned char c) {
                return std::isalnum(c) || c == '_' || c == '-' || c == '.';
            };
            if (!nameStart(static_cast<unsigned char>(s[0]))) return false;
            for (char c : s.substr(1))
                if (!nameChar(static_cast<unsigned char>(c))) return false;
            return true;
        }

        struct Attr {
            std::string name;
            std::string value;
        };

        // A property an attribute could not carry: a name that is not an XML name, or a value whose
        // spelling would not read back as itself. Written out long-hand with its type said aloud,
        // which is always unambiguous and is expected never to appear in a file anyone wrote by
        // hand in the designer.
        struct LongProp {
            std::string name;
            const char* type;
            std::string value;
        };

        // Every reserved attribute name, so a custom property colliding with one is caught rather
        // than silently overwriting it on load.
        bool IsReservedAttr(std::string_view name) {
            static constexpr std::string_view kNode[]{ "id", "name", "of", "start", "hidden",
                                                       "locked", "slot" };
            static constexpr std::string_view kLayout[]{ "mode", "axis", "width", "height",
                                                         "padding", "gap", "align", "justify",
                                                         "wrap", "columns", "minColumn", "rowGap",
                                                         "minSize", "maxSize", "aspectRatio",
                                                         "offsetStart", "offsetEnd", "constraintX",
                                                         "constraintY" };
            for (std::string_view n : kNode)   if (n == name) return true;
            for (std::string_view n : kLayout) if (n == name) return true;
            return PropFromName(name).has_value();
        }

        class Writer {
        public:
            explicit Writer(bool pretty) : m_Pretty(pretty) {}

            void Open(std::string_view tag, const std::vector<Attr>& attrs, bool leaf) {
                Indent();
                m_Out += '<';
                m_Out += tag;
                for (const Attr& a : attrs) {
                    m_Out += ' ';
                    m_Out += a.name;
                    m_Out += "=\"";
                    m_Out += EscapeAttr(a.value);
                    m_Out += '"';
                }
                if (leaf) { m_Out += "/>"; Break(); return; }
                m_Out += '>';
                Break();
                ++m_Depth;
            }

            // An element whose whole content is one string, on one line.
            void Leaf(std::string_view tag, const std::vector<Attr>& attrs, std::string_view body) {
                Indent();
                m_Out += '<';
                m_Out += tag;
                for (const Attr& a : attrs) {
                    m_Out += ' ';
                    m_Out += a.name;
                    m_Out += "=\"";
                    m_Out += EscapeAttr(a.value);
                    m_Out += '"';
                }
                m_Out += '>';
                m_Out += EscapeBody(body);
                m_Out += "</";
                m_Out += tag;
                m_Out += '>';
                Break();
            }

            void Close(std::string_view tag) {
                --m_Depth;
                Indent();
                m_Out += "</";
                m_Out += tag;
                m_Out += '>';
                Break();
            }

            std::string Take() { return std::move(m_Out); }

        private:
            void Indent() { if (m_Pretty) m_Out.append(static_cast<std::size_t>(m_Depth) * 2, ' '); }
            void Break()  { if (m_Pretty) m_Out += '\n'; }

            std::string m_Out;
            int m_Depth = 0;
            bool m_Pretty = true;
        };

        // ------------------------------------------------------------------ encoding a node

        // Only what differs from a default LayoutStyle, in the order the struct declares them —
        // eleven of the nineteen fields are at their default on 97-100% of real nodes, and a
        // declared order reads better than the alphabetical one a map would give.
        void LayoutAttrs(const layout::LayoutStyle& s, std::vector<Attr>& out) {
            const layout::LayoutStyle d{};
            if (s.mode != d.mode)               out.push_back({ "mode", LayoutModeName(s.mode) });
            if (s.axis != d.axis)               out.push_back({ "axis", AxisName(s.axis) });
            if (s.width != d.width)             out.push_back({ "width", SizeText(s.width) });
            if (s.height != d.height)           out.push_back({ "height", SizeText(s.height) });
            if (s.padding != d.padding)         out.push_back({ "padding", EdgesText(s.padding) });
            if (s.gap != d.gap)                 out.push_back({ "gap", Number(s.gap) });
            if (s.align != d.align)             out.push_back({ "align", AlignName(s.align) });
            if (s.justify != d.justify)         out.push_back({ "justify", JustifyName(s.justify) });
            if (s.wrap != d.wrap)               out.push_back({ "wrap", s.wrap ? "true" : "false" });
            if (s.columns != d.columns)         out.push_back({ "columns", Number(static_cast<f32>(s.columns)) });
            if (s.minColumn != d.minColumn)     out.push_back({ "minColumn", Number(s.minColumn) });
            if (s.rowGap != d.rowGap)           out.push_back({ "rowGap", Number(s.rowGap) });
            if (s.minSize != d.minSize)         out.push_back({ "minSize", Vec2Text(s.minSize) });
            // An unbounded max is the default and usually absent; when only one axis is bounded,
            // to_chars writes the other as "inf" and from_chars reads it back. JSON had to spell
            // this as null, having no way to say infinity at all.
            if (s.maxSize != d.maxSize)         out.push_back({ "maxSize", Vec2Text(s.maxSize) });
            if (s.aspectRatio != d.aspectRatio) out.push_back({ "aspectRatio", Number(s.aspectRatio) });
            if (s.offsetStart != d.offsetStart) out.push_back({ "offsetStart", Vec2Text(s.offsetStart) });
            if (s.offsetEnd != d.offsetEnd)     out.push_back({ "offsetEnd", Vec2Text(s.offsetEnd) });
            if (s.constraintX != d.constraintX) out.push_back({ "constraintX", ConstraintName(s.constraintX) });
            if (s.constraintY != d.constraintY) out.push_back({ "constraintY", ConstraintName(s.constraintY) });
        }

        // `skipText` is set when the text property is going into the element body instead, and
        // `skipSample` when a table of sample rows is going into a <sample> element of its own.
        void PropAttrs(const PropBag& props, std::vector<Attr>& out, std::vector<LongProp>& long_,
                       bool skipText = false, bool skipSample = false) {
            for (const auto& [prop, value] : props.Known()) {
                if (skipText && prop == Prop::Text) continue;
                if (skipSample && prop == Prop::Sample) continue;
                const ValueType declared = PropValueType(prop);
                if (auto attr = ValueToAttr(value, declared)) out.push_back({ PropName(prop), *attr });
                else if (auto explicit_ = ValueToAttr(value, ValueType::Unset))
                    long_.push_back({ PropName(prop), ValueTypeName(TypeOf(value)), *explicit_ });
            }
            for (const auto& [key, value] : props.Custom()) {
                // A state overlay is "hovered:fill" in the document and `hovered.fill` on disk: xml
                // reads ':' as a namespace separator, and '.' is an ordinary name character that no
                // property name uses.
                std::string name = key;
                std::replace(name.begin(), name.end(), ':', '.');

                auto attr = ValueToAttr(value, ValueType::Unset);
                if (attr && IsXmlName(name) && !IsReservedAttr(name)) {
                    out.push_back({ std::move(name), *attr });
                } else if (attr) {
                    long_.push_back({ key, ValueTypeName(TypeOf(value)), *attr });
                }
            }
        }

        // Which ids the file cannot do without: what an instance points at, what an override keys
        // on, what a property refers to, and the screen the app starts on. On Vaecord that is 6 of
        // 540 — scripts address nodes by name, so nothing outside the file needs the rest either.
        std::unordered_set<Uuid> ReferencedIds(const Document& document) {
            std::unordered_set<Uuid> out;
            if (document.ChosenStartScreen().Valid()) out.insert(document.ChosenStartScreen());
            for (Uuid rootId : document.Roots()) {
                for (Uuid id : document.Subtree(rootId)) {
                    const Node* node = document.Find(id);
                    if (!node) continue;
                    if (node->componentId.Valid()) out.insert(node->componentId);
                    for (const auto& [target, props] : node->overrides) {
                        out.insert(target);
                        for (const auto& [prop, value] : props.Known())
                            if (const Uuid* ref = std::get_if<Uuid>(&value)) out.insert(*ref);
                        for (const auto& [key, value] : props.Custom())
                            if (const Uuid* ref = std::get_if<Uuid>(&value)) out.insert(*ref);
                    }
                    for (const auto& [prop, value] : node->props.Known())
                        if (const Uuid* ref = std::get_if<Uuid>(&value)) out.insert(*ref);
                    for (const auto& [key, value] : node->props.Custom())
                        if (const Uuid* ref = std::get_if<Uuid>(&value)) out.insert(*ref);
                    // Every node inside a component master is referenced by definition: an
                    // instance's overrides key on exactly these ids.
                    if (node->IsComponent())
                        for (Uuid inner : document.Subtree(id)) out.insert(inner);
                }
            }
            return out;
        }

        void WriteNode(Writer& w, const Document& document, Uuid id,
                       const std::unordered_set<Uuid>& referenced, Uuid start, bool keepIds) {
            const Node* node = document.Find(id);
            if (!node) return;

            const char* tag = NodeKindName(node->kind);
            std::vector<Attr> attrs;
            std::vector<LongProp> long_;

            if (keepIds || referenced.contains(id)) attrs.push_back({ "id", id.ToString() });
            if (!node->name.empty())                attrs.push_back({ "name", node->name });
            if (id == start)                        attrs.push_back({ "start", "true" });
            if (node->componentId.Valid())          attrs.push_back({ "of", node->componentId.ToString() });

            LayoutAttrs(node->layout, attrs);

            // A label with a newline in it goes in the element body: `&#10;` in an attribute is
            // exactly the unreadable escaping this format exists to stop writing. Only when the
            // element has nothing else inside it, though — text interleaved with child elements is
            // legal xml and unreadable markup, and a multi-line label on a container is a case that
            // does not occur.
            const Value* textValue = node->props.Find(Prop::Text);
            const std::string* multiline = nullptr;
            if (textValue && node->children.empty() && node->overrides.empty()) {
                if (const auto* s = std::get_if<std::string>(textValue))
                    if (s->find('\n') != std::string::npos) multiline = s;
            }

            // Sample rows are a small table, and a table on one line behind `&#10;` escapes is
            // the unreadable form this format exists to stop writing. It gets an element, which is
            // also the only place it could go: the container it belongs to has the row template
            // inside it, so the body is already taken.
            const std::string* sample = nullptr;
            if (const Value* value = node->props.Find(Prop::Sample))
                if (const auto* text = std::get_if<std::string>(value); text && !text->empty())
                    sample = text;

            PropAttrs(node->props, attrs, long_, multiline != nullptr, sample != nullptr);
            // A property that needed the long form puts elements back inside, so the body is no
            // longer free; the label goes back to being an attribute.
            if (multiline && (sample || !long_.empty())) {
                attrs.push_back({ PropName(Prop::Text), EscapeLiteral(*multiline) });
                multiline = nullptr;
            }

            if (!node->visible) attrs.push_back({ "hidden", "true" });
            if (node->locked)   attrs.push_back({ "locked", "true" });
            if (node->slot)     attrs.push_back({ "slot", "true" });

            const bool hasChildren = !node->children.empty() || !node->overrides.empty()
                                   || !long_.empty() || sample != nullptr
                                   || !node->properties.empty();
            if (!hasChildren && multiline) { w.Leaf(tag, attrs, *multiline); return; }
            if (!hasChildren)              { w.Open(tag, attrs, true); return; }

            w.Open(tag, attrs, false);
            // The knobs a component exposes, before anything else inside it: they are what the
            // component is for, and a file reads as an outline of the app.
            for (const ComponentProperty& property : node->properties) {
                std::vector<Attr> pa{ { "name", property.name },
                                      { "type", std::string(ValueTypeName(property.type)) } };
                if (const auto text = ValueToAttr(property.defaultValue, property.type))
                    pa.push_back({ "default", *text });
                if (property.IsVariant()) {
                    std::string options;
                    for (const std::string& option : property.options) {
                        if (!options.empty()) options += ',';
                        options += option;
                    }
                    pa.push_back({ "options", options });
                }
                w.Open("property", pa, true);
            }
            if (sample) w.Leaf("sample", {}, *sample);
            for (const LongProp& p : long_)
                w.Open("prop", { { "name", p.name }, { "type", p.type }, { "value", p.value } }, true);
            for (const auto& [target, props] : node->overrides) {
                std::vector<Attr> oa{ { "target", target.ToString() } };
                std::vector<LongProp> ol;
                PropAttrs(props, oa, ol);
                if (ol.empty()) { w.Open("override", oa, true); continue; }
                w.Open("override", oa, false);
                for (const LongProp& p : ol)
                    w.Open("prop", { { "name", p.name }, { "type", p.type }, { "value", p.value } }, true);
                w.Close("override");
            }
            for (Uuid child : node->children)
                WriteNode(w, document, child, referenced, start, keepIds);
            w.Close(tag);
        }

    }

    std::string Serializer::ToXml(const Document& document, bool pretty,
                                  const LibrarySource* library, bool keepIds) {
        Writer w(pretty);

        // Everything the library can rebuild is named, not copied. A component the designer edited
        // no longer matches what the binary builds, so it is not in this set and is written in full
        // below — a fork lives in the file that forked it.
        std::unordered_set<Uuid> named;
        std::vector<Attr> root{ { "version", std::to_string(kFormatVersion) } };
        if (library) {
            root.push_back({ "library", std::string(library->Id()) + "@"
                                        + std::to_string(library->Version()) });
            for (Uuid component : library->Stock(document))
                for (Uuid node : document.Subtree(component)) named.insert(node);
        }
        root.push_back({ "theme", document.ActiveTheme() == Theme::Dark ? "dark" : "light" });
        w.Open("vae", root, false);

        // The library's own tokens are not written either. A document that left `accent` alone
        // gets it back from Install; one that recoloured it writes the new value over the top. The
        // case that needs saying out loud is a default the designer DELETED — silence would read
        // as "unchanged" and Install would hand it straight back on the next load, so a deletion
        // is written as one.
        const std::map<std::string, Token>* stock = library ? &library->Tokens() : nullptr;
        std::map<std::string, std::vector<Attr>> tokens;
        for (const auto& [name, token] : document.Tokens()) {
            if (stock) {
                const auto it = stock->find(name);
                if (it != stock->end() && it->second == token) continue;
            }
            std::vector<Attr> attrs{ { "name", name } };
            const auto light = ValueToAttr(token.light, ValueType::Unset);
            const auto dark  = ValueToAttr(token.dark, ValueType::Unset);
            // A token whose two themes agree says its value once, which is 10 of Vaecord's 26.
            if (light && dark && token.light == token.dark) {
                attrs.push_back({ "value", *dark });
            } else {
                if (dark)  attrs.push_back({ "dark", *dark });
                if (light) attrs.push_back({ "light", *light });
            }
            if (!token.description.empty()) attrs.push_back({ "desc", token.description });
            tokens.emplace(name, std::move(attrs));
        }
        if (stock)
            for (const auto& [name, token] : *stock)
                if (!document.FindToken(name))
                    tokens.emplace(name, std::vector<Attr>{ { "name", name }, { "removed", "true" } });

        if (!tokens.empty()) {
            w.Open("tokens", {}, false);
            for (const auto& [name, attrs] : tokens) w.Open("token", attrs, true);
            w.Close("tokens");
        }

        // Assets are the project's, but the project is one document today, so they travel with it.
        // Paths stay relative: an absolute one is a project that only opens on one person's disk.
        for (const Document::Asset& asset : document.Assets())
            w.Open("asset", { { "id", asset.id.ToString() },
                              { "name", asset.name },
                              { "path", asset.path } }, true);

        const std::unordered_set<Uuid> referenced = ReferencedIds(document);
        const Uuid start = document.ChosenStartScreen();
        for (Uuid id : document.Roots()) {
            if (named.contains(id)) continue;
            WriteNode(w, document, id, referenced, start, keepIds);
        }

        w.Close("vae");
        return w.Take();
    }


    // ------------------------------------------------------------------------ reading

    namespace {

        std::optional<layout::LayoutMode> LayoutModeFromName(std::string_view n) {
            if (n == "absolute") return layout::LayoutMode::Absolute;
            if (n == "stack")    return layout::LayoutMode::Stack;
            if (n == "grid")     return layout::LayoutMode::Grid;
            return std::nullopt;
        }
        std::optional<layout::Axis> AxisFromName(std::string_view n) {
            if (n == "row")    return layout::Axis::Row;
            if (n == "column") return layout::Axis::Column;
            return std::nullopt;
        }
        std::optional<layout::Align> AlignFromName(std::string_view n) {
            if (n == "start")   return layout::Align::Start;
            if (n == "center")  return layout::Align::Center;
            if (n == "end")     return layout::Align::End;
            if (n == "stretch") return layout::Align::Stretch;
            return std::nullopt;
        }
        std::optional<layout::Justify> JustifyFromName(std::string_view n) {
            if (n == "start")        return layout::Justify::Start;
            if (n == "center")       return layout::Justify::Center;
            if (n == "end")          return layout::Justify::End;
            if (n == "spaceBetween") return layout::Justify::SpaceBetween;
            if (n == "spaceAround")  return layout::Justify::SpaceAround;
            if (n == "spaceEvenly")  return layout::Justify::SpaceEvenly;
            return std::nullopt;
        }
        std::optional<layout::Constraint> ConstraintFromName(std::string_view n) {
            if (n == "start")    return layout::Constraint::Start;
            if (n == "end")      return layout::Constraint::End;
            if (n == "startEnd") return layout::Constraint::StartEnd;
            if (n == "center")   return layout::Constraint::Center;
            if (n == "scale")    return layout::Constraint::Scale;
            return std::nullopt;
        }

        std::optional<layout::Size> SizeFromText(std::string_view s) {
            if (s == "hug")  return layout::Size::Hug();
            if (s == "fill") return layout::Size::Fill();
            if (s.starts_with("fill ")) {
                if (auto w = ParseNumber(s.substr(5))) return layout::Size::Fill(*w);
                return std::nullopt;
            }
            if (s.ends_with("%")) {
                if (auto f = ParseNumber(s.substr(0, s.size() - 1)))
                    return layout::Size::Percent(*f / 100.0f);
                return std::nullopt;
            }
            if (auto px = ParseNumber(s)) return layout::Size::Px(*px);
            return std::nullopt;
        }

        std::optional<Edges> EdgesFromText(std::string_view s) {
            auto nums = Numbers(s);
            if (!nums) return std::nullopt;
            if (nums->size() == 1) return Edges{ (*nums)[0] };
            if (nums->size() == 2) return Edges{ (*nums)[0], (*nums)[1] };
            if (nums->size() == 4) return Edges{ (*nums)[0], (*nums)[1], (*nums)[2], (*nums)[3] };
            return std::nullopt;
        }

        std::optional<ValueType> ValueTypeFromName(std::string_view n) {
            for (u8 i = 0; i <= static_cast<u8>(ValueType::Bound); ++i)
                if (n == ValueTypeName(static_cast<ValueType>(i)))
                    return static_cast<ValueType>(i);
            return std::nullopt;
        }

        // A <prop> element says its own type, so it is read with that type rather than by shape.
        // This is the path a property takes when no attribute could have carried it.
        Value ValueFromLong(std::string_view text, std::string_view typeName) {
            const auto type = ValueTypeFromName(typeName);
            if (!type) return std::string(text);
            switch (*type) {
                case ValueType::Unset:   return {};
                case ValueType::Bool:    return text == "true";
                case ValueType::Number:  return ParseNumber(text).value_or(0.0f);
                case ValueType::Text:    return std::string(text);
                default: break;
            }
            return ValueFromAttr(text, *type);
        }

        // The reading half of the collection the writer builds. Everything it does not recognise is
        // an error rather than a shrug: a document written by a newer build is caught by the version
        // gate before it ever gets here, so an unknown name at this point is a typo or a bug, and
        // silently dropping it loses work.
        struct Reader {
            std::string error;
            u32 line = 0;

            bool Fail(std::string message, const pugi::xml_node& at) {
                // pugixml does not keep line numbers, so the offset is turned into one against the
                // source. Worth the scan: "unknown attribute 'witdh'" without a line is a grep.
                error = std::move(message);
                if (const char* name = at.name(); name && *name) error += " (in <" + std::string(name) + ">)";
                return false;
            }

            bool ReadLayout(const pugi::xml_attribute& a, layout::LayoutStyle& s, bool& handled) {
                const std::string_view n = a.name();
                const std::string_view v = a.value();
                handled = true;
                if (n == "mode") {
                    if (auto m = LayoutModeFromName(v)) { s.mode = *m; return true; }
                } else if (n == "axis") {
                    if (auto x = AxisFromName(v)) { s.axis = *x; return true; }
                } else if (n == "width") {
                    if (auto z = SizeFromText(v)) { s.width = *z; return true; }
                } else if (n == "height") {
                    if (auto z = SizeFromText(v)) { s.height = *z; return true; }
                } else if (n == "padding") {
                    if (auto e = EdgesFromText(v)) { s.padding = *e; return true; }
                } else if (n == "gap") {
                    if (auto f = ParseNumber(v)) { s.gap = *f; return true; }
                } else if (n == "align") {
                    if (auto x = AlignFromName(v)) { s.align = *x; return true; }
                } else if (n == "justify") {
                    if (auto x = JustifyFromName(v)) { s.justify = *x; return true; }
                } else if (n == "wrap") {
                    s.wrap = v == "true"; return true;
                } else if (n == "columns") {
                    if (auto f = ParseNumber(v)) { s.columns = static_cast<u16>(*f); return true; }
                } else if (n == "minColumn") {
                    if (auto f = ParseNumber(v)) { s.minColumn = *f; return true; }
                } else if (n == "rowGap") {
                    if (auto f = ParseNumber(v)) { s.rowGap = *f; return true; }
                } else if (n == "minSize") {
                    if (auto p = Vec2FromText(v)) { s.minSize = *p; return true; }
                } else if (n == "maxSize") {
                    if (auto p = Vec2FromText(v)) { s.maxSize = *p; return true; }
                } else if (n == "aspectRatio") {
                    if (auto f = ParseNumber(v)) { s.aspectRatio = *f; return true; }
                } else if (n == "offsetStart") {
                    if (auto p = Vec2FromText(v)) { s.offsetStart = *p; return true; }
                } else if (n == "offsetEnd") {
                    if (auto p = Vec2FromText(v)) { s.offsetEnd = *p; return true; }
                } else if (n == "constraintX") {
                    if (auto x = ConstraintFromName(v)) { s.constraintX = *x; return true; }
                } else if (n == "constraintY") {
                    if (auto x = ConstraintFromName(v)) { s.constraintY = *x; return true; }
                } else {
                    handled = false;
                    return true;
                }
                error = "attribute '" + std::string(n) + "' does not understand \"" + std::string(v) + "\"";
                return false;
            }
        };

    }

    bool Serializer::FromXml(std::string_view xml, Document& out, std::string* error,
                             const LibrarySource* library, bool merge) {
        const auto Fail = [&](const std::string& message) {
            if (error) *error = message;
            return false;
        };

        pugi::xml_document dom;
        const pugi::xml_parse_result parsed =
            dom.load_buffer(xml.data(), xml.size(), pugi::parse_default);
        if (!parsed) {
            // An offset is not a line number, and a line number is what makes a parse error
            // actionable, so it is counted here rather than reported raw.
            const auto offset = static_cast<std::size_t>(parsed.offset);
            const std::size_t line =
                1 + static_cast<std::size_t>(std::count(xml.begin(),
                                                        xml.begin() + std::min(offset, xml.size()),
                                                        '\n'));
            return Fail(std::string("line ") + std::to_string(line) + ": " + parsed.description());
        }

        const pugi::xml_node root = dom.child("vae");
        if (!root) return Fail("not a VAE document (no <vae> root)");

        // One format, so this is an equality check rather than a range. A file from either side of
        // the only format there is gets told which one it is instead of being half-read.
        const u32 version = static_cast<u32>(root.attribute("version").as_uint(0));
        if (version == 0) return Fail("missing format version");
        if (version != kFormatVersion)
            return Fail("document is format " + std::to_string(version) + "; this VAE reads "
                        + std::to_string(kFormatVersion));

        // Merging reads this file into whatever `out` already holds: the parts of a split project
        // are read one after another into a single document, and clearing between them would leave
        // only the last one.
        if (!merge) out.Clear();

        // The library goes in first so the document's own tokens and any forked component land on
        // top of it rather than under it. When merging it is already there, and installing it twice
        // would mint the ids it minted the first time.
        const pugi::xml_attribute libraryAttr = root.attribute("library");
        if (libraryAttr && !merge) {
            const std::string ref = libraryAttr.as_string();
            const std::size_t at = ref.rfind('@');
            const std::string id = at == std::string::npos ? ref : ref.substr(0, at);
            const u32 libraryVersion = at == std::string::npos
                ? 0u : static_cast<u32>(std::strtoul(ref.c_str() + at + 1, nullptr, 10));
            if (!library)
                return Fail("this document needs the '" + id + "' component library and none was "
                            "supplied to the loader");
            if (!library->Install(id, libraryVersion, out))
                return Fail("no component library '" + id + "' at version "
                            + std::to_string(libraryVersion));
        }

        out.SetTheme(std::string_view(root.attribute("theme").as_string("dark")) == "light"
                     ? Theme::Light : Theme::Dark);

        Reader reader;
        Uuid start = Uuid::Invalid();

        // Everything a node needs that is not in the tree shape: read into a Node, then handed to
        // the document once its whole subtree is known.
        const std::function<bool(const pugi::xml_node&, Uuid, std::vector<Node>&)> readNode =
            [&](const pugi::xml_node& el, Uuid parent, std::vector<Node>& nodes) -> bool {
            const auto kind = NodeKindFromName(el.name());
            if (!kind) { reader.error = "unknown element <" + std::string(el.name()) + ">"; return false; }

            Node node;
            node.kind = *kind;
            node.parent = parent;

            bool wantsStart = false;
            for (pugi::xml_attribute a : el.attributes()) {
                const std::string_view n = a.name();
                const std::string_view v = a.value();
                if (n == "id") { node.id = Uuid::FromString(std::string(v)); continue; }
                if (n == "name")   { node.name = v; continue; }
                if (n == "of")     { node.componentId = Uuid::FromString(std::string(v)); continue; }
                if (n == "start")  { wantsStart = v == "true"; continue; }
                if (n == "hidden") { node.visible = v != "true"; continue; }
                if (n == "locked") { node.locked = v == "true"; continue; }
                if (n == "slot")   { node.slot = v == "true"; continue; }

                bool handled = false;
                if (!reader.ReadLayout(a, node.layout, handled)) return false;
                if (handled) continue;

                // A dot is how a state overlay is spelled on disk; the document knows it as a colon.
                std::string key(n);
                std::replace(key.begin(), key.end(), '.', ':');
                if (const auto prop = PropFromName(key)) {
                    node.props.Set(*prop, ValueFromAttr(v, PropValueType(*prop)));
                } else if (key.find(':') != std::string::npos) {
                    node.props.Set(std::move(key), ValueFromAttr(v, ValueType::Unset));
                } else {
                    reader.error = "unknown attribute '" + std::string(n) + "' on <"
                                 + std::string(el.name()) + ">";
                    return false;
                }
            }

            // Ids are not written for nodes nothing references, so one is minted here. A node that
            // did carry an id keeps it: an override that keys on it has to still find it.
            if (!node.id.Valid()) node.id = Uuid();
            if (wantsStart) start = node.id;

            // A label with a newline in it lives in the element body.
            if (const char* body = el.child_value(); body && *body)
                node.props.Set(Prop::Text, std::string(body));

            std::vector<const pugi::xml_node*> childElements;
            std::vector<pugi::xml_node> kids;
            for (pugi::xml_node child : el.children()) {
                const std::string_view tag = child.name();
                if (tag.empty()) continue;                  // the text body, already read
                if (tag == "sample") {
                    node.props.Set(Prop::Sample, std::string(child.child_value()));
                    continue;
                }
                if (tag == "property") {
                    ComponentProperty property;
                    property.name = child.attribute("name").as_string();
                    property.type = ValueTypeFromName(child.attribute("type").as_string("text"))
                                        .value_or(ValueType::Text);
                    if (const auto attr = child.attribute("default"))
                        property.defaultValue = ValueFromAttr(attr.as_string(), property.type);
                    const std::string_view options = child.attribute("options").as_string();
                    for (std::size_t at = 0; at < options.size(); ) {
                        const std::size_t comma = options.find(',', at);
                        const std::size_t end = comma == std::string_view::npos ? options.size() : comma;
                        if (end > at) property.options.emplace_back(options.substr(at, end - at));
                        at = end + 1;
                    }
                    if (!property.name.empty()) node.properties.push_back(std::move(property));
                    continue;
                }
                if (tag == "prop") {
                    const std::string name = child.attribute("name").as_string();
                    const Value value = ValueFromLong(child.attribute("value").as_string(),
                                                      child.attribute("type").as_string("string"));
                    if (const auto prop = PropFromName(name)) node.props.Set(*prop, value);
                    else node.props.Set(name, value);
                    continue;
                }
                if (tag == "override") {
                    const Uuid target = Uuid::FromString(child.attribute("target").as_string());
                    PropBag bag;
                    for (pugi::xml_attribute a : child.attributes()) {
                        const std::string_view n = a.name();
                        if (n == "target") continue;
                        std::string key(n);
                        std::replace(key.begin(), key.end(), '.', ':');
                        if (const auto prop = PropFromName(key))
                            bag.Set(*prop, ValueFromAttr(a.value(), PropValueType(*prop)));
                        else
                            bag.Set(std::move(key), ValueFromAttr(a.value(), ValueType::Unset));
                    }
                    for (pugi::xml_node p : child.children("prop"))
                        bag.Set(std::string(p.attribute("name").as_string()),
                                ValueFromLong(p.attribute("value").as_string(),
                                              p.attribute("type").as_string("string")));
                    node.overrides[target] = std::move(bag);
                    continue;
                }
                kids.push_back(child);
            }

            const Uuid id = node.id;
            nodes.push_back(std::move(node));
            const std::size_t self = nodes.size() - 1;
            std::vector<Uuid> children;
            for (const pugi::xml_node& child : kids) {
                const std::size_t before = nodes.size();
                if (!readNode(child, id, nodes)) return false;
                children.push_back(nodes[before].id);
            }
            nodes[self].children = std::move(children);
            return true;
        };

        std::vector<Node> nodes;
        for (pugi::xml_node el : root.children()) {
            const std::string_view tag = el.name();
            if (tag.empty()) continue;
            if (tag == "tokens") {
                for (pugi::xml_node t : el.children("token")) {
                    // A token the file says is gone: the library installed it a moment ago, and
                    // this is the document taking it back out.
                    if (t.attribute("removed").as_bool(false)) {
                        out.RemoveToken(t.attribute("name").as_string());
                        continue;
                    }
                    Token token;
                    if (const pugi::xml_attribute both = t.attribute("value")) {
                        token.dark = token.light = ValueFromAttr(both.as_string(), ValueType::Unset);
                    } else {
                        if (const auto d = t.attribute("dark"))
                            token.dark = ValueFromAttr(d.as_string(), ValueType::Unset);
                        if (const auto l = t.attribute("light"))
                            token.light = ValueFromAttr(l.as_string(), ValueType::Unset);
                    }
                    token.description = t.attribute("desc").as_string();
                    out.SetToken(t.attribute("name").as_string(), std::move(token));
                }
                continue;
            }
            if (tag == "asset") {
                out.AddAsset(el.attribute("name").as_string(), el.attribute("path").as_string(),
                             Uuid::FromString(el.attribute("id").as_string()));
                continue;
            }
            if (!readNode(el, Uuid::Invalid(), nodes)) return Fail(reader.error);
        }

        // A component the file carries and the library also built is a fork: the file wins, and it
        // carries the whole subtree, so drop the installed one rather than trying to merge them.
        for (const Node& node : nodes)
            if (!node.parent.Valid() && out.Contains(node.id)) out.DeleteNode(node.id);

        // InsertNode appends to the parent's child list, so the lists are rebuilt afterwards rather
        // than carried in — otherwise every child would appear twice.
        std::vector<std::pair<Uuid, std::vector<Uuid>>> hierarchy;
        for (auto& node : nodes) {
            hierarchy.emplace_back(node.id, node.children);
            node.children.clear();
            out.InsertNode(std::move(node));
        }
        for (const auto& [id, children] : hierarchy)
            if (Node* node = out.Find(id)) node->children = children;

        if (start.Valid()) out.SetStartScreen(start);
        return true;
    }

}
