#include "vaepch.h"
#include "vae/doc/Serializer.h"

#include "vae/base/FileSystem.h"

#include <nlohmann/json.hpp>

namespace vae::doc {

    using json = nlohmann::json;
    namespace fs = std::filesystem;

    namespace {

        // Values are written tagged. The alternative — inferring the type from the JSON shape —
        // cannot tell a colour from a 4-number array, or a token reference from a plain string,
        // and would silently change a document's meaning on the next load.
        json EncodeValue(const Value& value) {
            json out;
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    out = nullptr;
                } else if constexpr (std::is_same_v<T, bool>) {
                    out = { { "type", "bool" }, { "value", v } };
                } else if constexpr (std::is_same_v<T, f32>) {
                    out = { { "type", "number" }, { "value", v } };
                } else if constexpr (std::is_same_v<T, Vec2>) {
                    out = { { "type", "vec2" }, { "value", { v.x, v.y } } };
                } else if constexpr (std::is_same_v<T, Color>) {
                    out = { { "type", "color" }, { "value", { v.r, v.g, v.b, v.a } } };
                } else if constexpr (std::is_same_v<T, std::string>) {
                    out = { { "type", "string" }, { "value", v } };
                } else if constexpr (std::is_same_v<T, Uuid>) {
                    out = { { "type", "node" }, { "value", v.ToString() } };
                } else if constexpr (std::is_same_v<T, AssetRef>) {
                    out = { { "type", "asset" }, { "value", v.id.ToString() } };
                } else if constexpr (std::is_same_v<T, TokenRef>) {
                    out = { { "type", "token" }, { "value", v.name } };
                } else if constexpr (std::is_same_v<T, Binding>) {
                    out = { { "type", "binding" }, { "value", v.expression } };
                }
            }, value);
            return out;
        }

        Value DecodeValue(const json& node) {
            if (!node.is_object() || !node.contains("type")) return {};
            const std::string type = node.value("type", "");
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
            return { { "mode", SizeModeName(size.mode) }, { "value", size.value } };
        }

        layout::Size DecodeSize(const json& node) {
            layout::Size size;
            if (!node.is_object()) return size;
            size.mode = SizeModeFromName(node.value("mode", "hug"));
            size.value = node.value("value", 0.0f);
            return size;
        }

        json EncodeLayout(const layout::LayoutStyle& style) {
            json out;
            out["mode"] = style.mode == layout::LayoutMode::Stack ? "stack"
                        : style.mode == layout::LayoutMode::Grid  ? "grid" : "absolute";
            out["axis"] = style.axis == layout::Axis::Row ? "row" : "column";
            out["width"] = EncodeSize(style.width);
            out["height"] = EncodeSize(style.height);
            out["padding"] = { style.padding.left, style.padding.top,
                               style.padding.right, style.padding.bottom };
            out["gap"] = style.gap;
            out["align"] = static_cast<u8>(style.align);
            out["justify"] = static_cast<u8>(style.justify);
            out["wrap"] = style.wrap;
            out["columns"] = style.columns;
            out["minColumn"] = style.minColumn;
            out["rowGap"] = style.rowGap;
            out["minSize"] = { style.minSize.x, style.minSize.y };
            // Infinity is not representable in JSON, so an unbounded max is written as null.
            out["maxSize"] = { std::isfinite(style.maxSize.x) ? json(style.maxSize.x) : json(nullptr),
                               std::isfinite(style.maxSize.y) ? json(style.maxSize.y) : json(nullptr) };
            out["aspectRatio"] = style.aspectRatio;
            out["offsetStart"] = { style.offsetStart.x, style.offsetStart.y };
            out["offsetEnd"] = { style.offsetEnd.x, style.offsetEnd.y };
            out["constraintX"] = static_cast<u8>(style.constraintX);
            out["constraintY"] = static_cast<u8>(style.constraintY);
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

    std::string Serializer::ToJson(const Document& document, bool pretty) {
        json root;
        root["format"] = "vae.document";
        root["version"] = kFormatVersion;
        root["theme"] = document.ActiveTheme() == Theme::Dark ? "dark" : "light";
        if (document.StartScreen().Valid()) root["start"] = document.StartScreen().ToString();

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
        for (Uuid id : document.Roots()) roots.push_back(id.ToString());
        root["roots"] = std::move(roots);

        // Nodes are written as an array in document order rather than a map, so a diff shows a
        // moved node as a move instead of two unrelated key changes.
        json nodes = json::array();
        for (Uuid rootId : document.Roots()) {
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

                entry["layout"] = EncodeLayout(node->layout);
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

    bool Serializer::FromJson(std::string_view text, Document& out, std::string* error) {
        auto Fail = [&](const std::string& message) {
            if (error) *error = message;
            return false;
        };

        json root = json::parse(text, nullptr, false);
        if (root.is_discarded()) return Fail("not valid JSON");
        if (root.value("format", "") != "vae.document") return Fail("not a VAE document");

        const u32 version = root.value("version", 0u);
        if (version == 0) return Fail("missing format version");
        if (version > kFormatVersion)
            return Fail("document was written by a newer VAE (format " + std::to_string(version)
                        + ", this build reads " + std::to_string(kFormatVersion) + ")");

        out.Clear();
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

        // Roots are whatever the file says, not whatever fell out of insertion order.
        return true;
    }

    bool Serializer::Save(const Document& document, const fs::path& path) {
        return FileSystem::WriteText(path, ToJson(document));
    }

    bool Serializer::Load(const fs::path& path, Document& out, std::string* error) {
        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        return FromJson(*text, out, error);
    }

    // ---------------------------------------------------------------- Project

    bool Project::Save(const Project& project, const fs::path& path) {
        json root;
        root["format"] = "vae.project";
        root["version"] = Serializer::kFormatVersion;
        root["name"] = project.name;
        root["scriptLanguage"] = project.scriptLanguage;
        root["screens"] = project.screens;
        root["components"] = project.components;
        root["fontDirs"] = project.fontDirs;
        root["targetResolution"] = { project.targetResolution.x, project.targetResolution.y };
        return FileSystem::WriteText(path, root.dump(2));
    }

    bool Project::Load(const fs::path& path, Project& out, std::string* error) {
        auto text = FileSystem::ReadText(path);
        if (!text) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        json root = json::parse(*text, nullptr, false);
        if (root.is_discarded() || root.value("format", "") != "vae.project") {
            if (error) *error = "not a VAE project file";
            return false;
        }
        if (root.value("version", 0u) > Serializer::kFormatVersion) {
            if (error) *error = "project was written by a newer VAE";
            return false;
        }

        out.name = root.value("name", "Untitled");
        out.scriptLanguage = root.value("scriptLanguage", "lua");
        out.root = path.parent_path();
        out.screens = root.value("screens", std::vector<std::string>{});
        out.components = root.value("components", std::vector<std::string>{});
        out.fontDirs = root.value("fontDirs", std::vector<std::string>{});
        if (root.contains("targetResolution") && root["targetResolution"].size() == 2)
            out.targetResolution = { root["targetResolution"][0].get<f32>(),
                                     root["targetResolution"][1].get<f32>() };
        return true;
    }

}
