#include "vaepch.h"
#include "vae/doc/Serializer.h"

#include "vae/doc/ValueText.h"

#include <nlohmann/json.hpp>

#include <cmath>

// The JSON half of the document codec: format 2 and everything before it, and the shape a Figma
// import arrives in. No longer what a project saves as — see XmlCodec.cpp and
// design/xml-format.md — but read forever, because every file that exists is written in it.
namespace vae::doc {

    using json = nlohmann::json;

    namespace {

        // The scalar spellings — sigils, hex colours, shortest-round-trip numbers — are shared
        // with the XML codec and live in doc/ValueText.h. What is local to JSON is where a value
        // goes once it is spelled: an object, an array or a bare literal.
        using namespace vae::doc::text;

        // Every number in a document is an f32, and nlohmann's only float type is a double. Handing
        // it the promoted value writes 0.6f as 0.6000000238418579; handing it the double nearest to
        // the shortest f32 spelling writes 0.6, and narrowing that back recovers the same bits.
        json Num(f32 value) { return NumberAsDouble(value); }

        json EncodeValue(const Value& value) {
            json out;
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    out = nullptr;
                } else if constexpr (std::is_same_v<T, bool>) {
                    out = v;
                } else if constexpr (std::is_same_v<T, f32>) {
                    out = Num(v);
                } else if constexpr (std::is_same_v<T, Vec2>) {
                    out = { Num(v.x), Num(v.y) };       // two numbers: a vector
                } else if constexpr (std::is_same_v<T, Color>) {
                    if (auto hex = ColorToHex(v)) out = *hex;
                    else out = { Num(v.r), Num(v.g), Num(v.b), Num(v.a) };  // four: a colour
                } else if constexpr (std::is_same_v<T, std::string>) {
                    out = EscapeLiteral(v);
                } else if constexpr (std::is_same_v<T, Uuid>) {
                    out = { { "node", v.ToString() } };
                } else if constexpr (std::is_same_v<T, AssetRef>) {
                    out = { { "asset", v.id.ToString() } };
                } else if constexpr (std::is_same_v<T, TokenRef>) {
                    out = "@" + v.name;
                } else if constexpr (std::is_same_v<T, Binding>) {
                    out = "=" + v.expression;
                }
            }, value);
            return out;
        }

        // Format 1 wrote every value as {"type","value"}. Still read, so opening an old project
        // works; never written.
        Value DecodeTaggedValue(const json& node) {
            const std::string type = node.value("type", "");
            if (!node.contains("value")) return {};
            const json& v = node["value"];

            if (type == "bool")    return v.get<bool>();
            if (type == "number")  return v.get<f32>();
            if (type == "vec2")    return Vec2{ v[0].get<f32>(), v[1].get<f32>() };
            if (type == "color")   return Color{ v[0].get<f32>(), v[1].get<f32>(),
                                                 v[2].get<f32>(), v[3].get<f32>() };
            if (type == "string")  return v.get<std::string>();
            if (type == "node")    return Uuid::FromString(v.get<std::string>());
            if (type == "asset")   return AssetRef{ Uuid::FromString(v.get<std::string>()) };
            if (type == "token")   return TokenRef{ v.get<std::string>() };
            if (type == "binding") return Binding{ v.get<std::string>() };
            return {};
        }

        Value DecodeValue(const json& node) {
            if (node.is_null())    return {};
            if (node.is_boolean()) return node.get<bool>();
            if (node.is_number())  return node.get<f32>();

            if (node.is_array()) {
                if (node.size() == 2 && node[0].is_number() && node[1].is_number())
                    return Vec2{ node[0].get<f32>(), node[1].get<f32>() };
                if (node.size() == 4)
                    return Color{ node[0].get<f32>(), node[1].get<f32>(),
                                  node[2].get<f32>(), node[3].get<f32>() };
                return {};
            }

            if (node.is_string()) {
                const std::string text = node.get<std::string>();
                if (text.empty()) return text;
                switch (text[0]) {
                    case '@': return TokenRef{ text.substr(1) };
                    case '=': return Binding{ text.substr(1) };
                    case '$': return text.substr(1);
                    case '#': if (auto c = ColorFromHex(text)) return *c;
                              return text;              // not a colour after all: a literal
                    default:  return text;
                }
            }

            if (node.is_object()) {
                if (node.contains("type"))  return DecodeTaggedValue(node);
                if (node.contains("asset")) return AssetRef{ Uuid::FromString(node["asset"].get<std::string>()) };
                if (node.contains("node"))  return Uuid::FromString(node["node"].get<std::string>());
            }
            return {};
        }

        json EncodeProps(const PropBag& props) {
            json out = json::object();
            for (const auto& [prop, value] : props.Known()) out[PropName(prop)] = EncodeValue(value);
            if (!props.Custom().empty()) {
                json custom = json::object();
                for (const auto& [key, value] : props.Custom()) custom[key] = EncodeValue(value);
                out["$custom"] = std::move(custom);
            }
            return out;
        }

        PropBag DecodeProps(const json& node) {
            PropBag props;
            if (!node.is_object()) return props;
            for (const auto& [key, value] : node.items()) {
                if (key == "$custom") {
                    for (const auto& [customKey, customValue] : value.items())
                        props.Set(customKey, DecodeValue(customValue));
                    continue;
                }
                if (auto prop = PropFromName(key)) props.Set(*prop, DecodeValue(value));
                // Unknown keys are dropped rather than guessed at: a document written by a newer
                // build is refused by the version check before ever reaching here.
            }
            return props;
        }

        const char* SizeModeName(layout::SizeMode mode) {
            switch (mode) {
                case layout::SizeMode::Fixed:   return "fixed";
                case layout::SizeMode::Hug:     return "hug";
                case layout::SizeMode::Fill:    return "fill";
                case layout::SizeMode::Percent: return "percent";
            }
            return "hug";
        }

        layout::SizeMode SizeModeFromName(std::string_view name) {
            if (name == "fixed")   return layout::SizeMode::Fixed;
            if (name == "fill")    return layout::SizeMode::Fill;
            if (name == "percent") return layout::SizeMode::Percent;
            return layout::SizeMode::Hug;
        }

        json EncodeSize(const layout::Size& size) {
            return { { "mode", SizeModeName(size.mode) }, { "value", Num(size.value) } };
        }

        layout::Size DecodeSize(const json& node) {
            layout::Size size;
            if (!node.is_object()) return size;
            size.mode = SizeModeFromName(node.value("mode", "hug"));
            size.value = node.value("value", 0.0f);
            return size;
        }

        // Only what differs from a default LayoutStyle. Eleven of the nineteen fields are at
        // their default on 97-100% of real nodes, and writing them anyway was 40% of a document.
        // The decoder already reads every field through a default, so an absent key means what it
        // always meant.
        json EncodeLayout(const layout::LayoutStyle& style) {
            const layout::LayoutStyle d{};
            json out = json::object();

            if (style.mode != d.mode)
                out["mode"] = style.mode == layout::LayoutMode::Stack ? "stack"
                            : style.mode == layout::LayoutMode::Grid  ? "grid" : "absolute";
            if (style.axis != d.axis)
                out["axis"] = style.axis == layout::Axis::Row ? "row" : "column";
            if (style.width != d.width)   out["width"] = EncodeSize(style.width);
            if (style.height != d.height) out["height"] = EncodeSize(style.height);
            if (style.padding != d.padding)
                out["padding"] = { Num(style.padding.left), Num(style.padding.top),
                                   Num(style.padding.right), Num(style.padding.bottom) };
            if (style.gap != d.gap)             out["gap"] = Num(style.gap);
            if (style.align != d.align)         out["align"] = static_cast<u8>(style.align);
            if (style.justify != d.justify)     out["justify"] = static_cast<u8>(style.justify);
            if (style.wrap != d.wrap)           out["wrap"] = style.wrap;
            if (style.columns != d.columns)     out["columns"] = style.columns;
            if (style.minColumn != d.minColumn) out["minColumn"] = Num(style.minColumn);
            if (style.rowGap != d.rowGap)       out["rowGap"] = Num(style.rowGap);
            if (style.minSize != d.minSize)     out["minSize"] = { Num(style.minSize.x), Num(style.minSize.y) };
            // Infinity is not representable in JSON, so an unbounded max is written as null — and
            // unbounded on both axes is the default, so it is usually not written at all.
            if (style.maxSize != d.maxSize)
                out["maxSize"] = { std::isfinite(style.maxSize.x) ? Num(style.maxSize.x) : json(nullptr),
                                   std::isfinite(style.maxSize.y) ? Num(style.maxSize.y) : json(nullptr) };
            if (style.aspectRatio != d.aspectRatio) out["aspectRatio"] = Num(style.aspectRatio);
            if (style.offsetStart != d.offsetStart)
                out["offsetStart"] = { Num(style.offsetStart.x), Num(style.offsetStart.y) };
            if (style.offsetEnd != d.offsetEnd)
                out["offsetEnd"] = { Num(style.offsetEnd.x), Num(style.offsetEnd.y) };
            if (style.constraintX != d.constraintX) out["constraintX"] = static_cast<u8>(style.constraintX);
            if (style.constraintY != d.constraintY) out["constraintY"] = static_cast<u8>(style.constraintY);
            return out;
        }

        layout::LayoutStyle DecodeLayout(const json& node) {
            layout::LayoutStyle style;
            if (!node.is_object()) return style;

            const std::string mode = node.value("mode", "absolute");
            style.mode = mode == "stack" ? layout::LayoutMode::Stack
                       : mode == "grid"  ? layout::LayoutMode::Grid
                                         : layout::LayoutMode::Absolute;
            style.axis = node.value("axis", "column") == "row" ? layout::Axis::Row
                                                               : layout::Axis::Column;
            if (node.contains("width"))  style.width = DecodeSize(node["width"]);
            if (node.contains("height")) style.height = DecodeSize(node["height"]);
            if (node.contains("padding") && node["padding"].size() == 4) {
                const auto& p = node["padding"];
                style.padding = Edges{ p[0].get<f32>(), p[1].get<f32>(),
                                       p[2].get<f32>(), p[3].get<f32>() };
            }
            style.gap = node.value("gap", 0.0f);
            style.align = static_cast<layout::Align>(node.value("align", 0));
            style.justify = static_cast<layout::Justify>(node.value("justify", 0));
            style.wrap = node.value("wrap", false);
            style.columns = node.value("columns", static_cast<u16>(0));
            style.minColumn = node.value("minColumn", 160.0f);
            style.rowGap = node.value("rowGap", 0.0f);
            if (node.contains("minSize") && node["minSize"].size() == 2)
                style.minSize = { node["minSize"][0].get<f32>(), node["minSize"][1].get<f32>() };
            if (node.contains("maxSize") && node["maxSize"].size() == 2) {
                style.maxSize.x = node["maxSize"][0].is_null() ? layout::kUnbounded
                                                               : node["maxSize"][0].get<f32>();
                style.maxSize.y = node["maxSize"][1].is_null() ? layout::kUnbounded
                                                               : node["maxSize"][1].get<f32>();
            }
            style.aspectRatio = node.value("aspectRatio", 0.0f);
            if (node.contains("offsetStart") && node["offsetStart"].size() == 2)
                style.offsetStart = { node["offsetStart"][0].get<f32>(), node["offsetStart"][1].get<f32>() };
            if (node.contains("offsetEnd") && node["offsetEnd"].size() == 2)
                style.offsetEnd = { node["offsetEnd"][0].get<f32>(), node["offsetEnd"][1].get<f32>() };
            style.constraintX = static_cast<layout::Constraint>(node.value("constraintX", 0));
            style.constraintY = static_cast<layout::Constraint>(node.value("constraintY", 0));
            return style;
        }

    }

    std::string Serializer::ToJson(const Document& document, bool pretty,
                                   const LibrarySource* library) {
        json root;
        root["format"] = "vae.document";
        root["version"] = kJsonFormatVersion;
        root["theme"] = document.ActiveTheme() == Theme::Dark ? "dark" : "light";

        // Everything the library can rebuild is named, not copied. A component the designer edited
        // no longer matches what the binary builds, so it is not in this set and is written in full
        // below — a fork lives in the file that forked it.
        std::unordered_set<Uuid> named;
        if (library) {
            root["library"] = { { "id", std::string(library->Id()) },
                                { "version", library->Version() } };
            for (Uuid component : library->Stock(document))
                for (Uuid node : document.Subtree(component)) named.insert(node);
        }
        if (document.ChosenStartScreen().Valid())
            root["start"] = document.ChosenStartScreen().ToString();

        json tokens = json::object();
        for (const auto& [name, token] : document.Tokens()) {
            json entry;
            entry["light"] = EncodeValue(token.light);
            entry["dark"] = EncodeValue(token.dark);
            if (!token.description.empty()) entry["description"] = token.description;
            tokens[name] = std::move(entry);
        }
        root["tokens"] = std::move(tokens);

        // Assets are the project's, but the project is one document today, so they travel with it.
        // Paths are relative: an absolute one is a project that only opens on the machine it was
        // made on.
        if (!document.Assets().empty()) {
            json assets = json::array();
            for (const Document::Asset& asset : document.Assets())
                assets.push_back({ { "id", asset.id.ToString() },
                                   { "name", asset.name },
                                   { "path", asset.path } });
            root["assets"] = std::move(assets);
        }

        json roots = json::array();
        for (Uuid id : document.Roots())
            if (!named.contains(id)) roots.push_back(id.ToString());
        root["roots"] = std::move(roots);

        // Nodes are written as an array in document order rather than a map, so a diff shows a
        // moved node as a move instead of two unrelated key changes.
        json nodes = json::array();
        for (Uuid rootId : document.Roots()) {
            if (named.contains(rootId)) continue;
            for (Uuid id : document.Subtree(rootId)) {
                const Node* node = document.Find(id);
                if (!node) continue;

                json entry;
                entry["id"] = node->id.ToString();
                entry["kind"] = NodeKindName(node->kind);
                entry["name"] = node->name;
                if (node->parent.Valid()) entry["parent"] = node->parent.ToString();

                json children = json::array();
                for (Uuid child : node->children) children.push_back(child.ToString());
                if (!children.empty()) entry["children"] = std::move(children);

                if (json layout = EncodeLayout(node->layout); !layout.empty())
                    entry["layout"] = std::move(layout);
                if (!node->props.Empty()) entry["props"] = EncodeProps(node->props);
                if (!node->visible) entry["visible"] = false;
                if (node->locked)   entry["locked"] = true;
                if (node->slot)     entry["slot"] = true;
                if (node->componentId.Valid()) entry["componentId"] = node->componentId.ToString();

                if (!node->overrides.empty()) {
                    json overrides = json::object();
                    for (const auto& [target, props] : node->overrides)
                        overrides[target.ToString()] = EncodeProps(props);
                    entry["overrides"] = std::move(overrides);
                }
                nodes.push_back(std::move(entry));
            }
        }
        root["nodes"] = std::move(nodes);

        return pretty ? root.dump(2) : root.dump();
    }

    bool Serializer::FromJson(std::string_view text, Document& out, std::string* error,
                              const LibrarySource* library) {
        auto Fail = [&](const std::string& message) {
            if (error) *error = message;
            return false;
        };

        json root = json::parse(text, nullptr, false);
        if (root.is_discarded()) return Fail("not valid JSON");
        if (root.value("format", "") != "vae.document") return Fail("not a VAE document");

        const u32 version = root.value("version", 0u);
        if (version == 0) return Fail("missing format version");
        // A JSON file claiming format 3 is a contradiction — format 3 is markup — so this reads as
        // far as 2 and refuses anything above it rather than guessing.
        if (version > kJsonFormatVersion)
            return Fail("document was written by a newer VAE (JSON format "
                        + std::to_string(version) + "; this reader handles "
                        + std::to_string(kJsonFormatVersion) + " and older — format 3 is XML)");

        out.Clear();

        // The library goes in first so the document's own tokens and any forked component land on
        // top of it rather than under it.
        const bool referencesLibrary = root.contains("library");
        if (referencesLibrary) {
            const json& ref = root["library"];
            const std::string id = ref.value("id", "");
            const u32 libraryVersion = ref.value("version", 0u);
            if (!library)
                return Fail("this document needs the '" + id + "' component library and none was "
                            "supplied to the loader");
            if (!library->Install(id, libraryVersion, out))
                return Fail("no component library '" + id + "' at version "
                            + std::to_string(libraryVersion));
        }

        out.SetTheme(root.value("theme", "dark") == "light" ? Theme::Light : Theme::Dark);
        // Set before the nodes exist, which is fine: StartScreen resolves through the document and
        // falls back to the first screen when the id names nothing.
        if (root.contains("start"))
            out.SetStartScreen(Uuid::FromString(root["start"].get<std::string>()));

        if (root.contains("tokens")) {
            for (const auto& [name, entry] : root["tokens"].items()) {
                Token token;
                if (entry.contains("light")) token.light = DecodeValue(entry["light"]);
                if (entry.contains("dark"))  token.dark = DecodeValue(entry["dark"]);
                token.description = entry.value("description", "");
                out.SetToken(name, std::move(token));
            }
        }

        if (root.contains("assets")) {
            for (const auto& entry : root["assets"])
                out.AddAsset(entry.value("name", std::string{}), entry.value("path", std::string{}),
                             Uuid::FromString(entry.value("id", std::string{})));
        }

        // Two passes: build every node first, then wire the hierarchy, because a child can appear
        // before its parent in a hand-edited file.
        std::vector<Node> nodes;
        if (root.contains("nodes")) {
            for (const auto& entry : root["nodes"]) {
                Node node;
                node.id = Uuid::FromString(entry.value("id", ""));
                if (!node.id.Valid()) return Fail("node with a missing or invalid id");

                node.kind = NodeKindFromName(entry.value("kind", "frame")).value_or(NodeKind::Frame);
                node.name = entry.value("name", "");
                if (entry.contains("parent")) node.parent = Uuid::FromString(entry["parent"].get<std::string>());
                if (entry.contains("children"))
                    for (const auto& child : entry["children"])
                        node.children.push_back(Uuid::FromString(child.get<std::string>()));

                node.layout = DecodeLayout(entry.value("layout", json::object()));
                if (entry.contains("props")) node.props = DecodeProps(entry["props"]);
                node.visible = entry.value("visible", true);
                node.locked = entry.value("locked", false);
                node.slot = entry.value("slot", false);
                if (entry.contains("componentId"))
                    node.componentId = Uuid::FromString(entry["componentId"].get<std::string>());
                if (entry.contains("overrides"))
                    for (const auto& [target, props] : entry["overrides"].items())
                        node.overrides[Uuid::FromString(target)] = DecodeProps(props);

                nodes.push_back(std::move(node));
            }
        }

        // A component the file carries and the library also built is a fork: the file wins, and it
        // carries the whole subtree, so drop the installed one rather than trying to merge them.
        for (const Node& node : nodes)
            if (!node.parent.Valid() && out.Contains(node.id)) out.DeleteNode(node.id);

        // InsertNode appends to the parent's child list, so clear the lists first and let the
        // insertion order rebuild them; otherwise every child would appear twice.
        std::vector<std::pair<Uuid, std::vector<Uuid>>> hierarchy;
        for (auto& node : nodes) {
            hierarchy.emplace_back(node.id, node.children);
            node.children.clear();
            out.InsertNode(std::move(node));
        }
        for (const auto& [id, children] : hierarchy)
            if (Node* node = out.Find(id)) node->children = children;

        // A document written before format 2 carries its own copy of the catalog. Fold it back
        // into a reference now, so the size and the "a fix to Button reaches you" both apply to
        // files that already exist rather than only to new ones.
        if (!referencesLibrary && library) {
            if (const u32 folded = library->Adopt(out))
                VAE_CORE_INFO("adopted {} standard components into the shared library", folded);
        }

        // Roots are whatever the file says, not whatever fell out of insertion order.
        return true;
    }

}
