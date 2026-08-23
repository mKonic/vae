#include "vaepch.h"
#include "vae/gen/Emit.h"
#include "vae/text/FontDB.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace vae::gen {

    namespace {

        bool IsKeyword(std::string_view word) {
            static const std::set<std::string_view, std::less<>> kKeywords{
                "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
                "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
                "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
                "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
                "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
                "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
                "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
                "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
                "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
                "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
                "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
                "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
                // Not keywords, but shadowing them in the emitted scope is asking for trouble.
                "b", "document", "style",
            };
            return kKeywords.contains(word);
        }

        // A C++ identifier derived from the designer's name, so the generated file reads as the
        // screen it came from. Collisions get a number rather than a hash: `card` and `card2` say
        // more about a document than two hex strings do.
        struct Names {
            std::unordered_map<u64, std::string> byId;
            std::map<std::string, int> used;

            std::string Take(std::string_view name, std::string_view fallback) {
                std::string identifier;
                for (const char c : name) {
                    if (std::isalnum(static_cast<unsigned char>(c))) identifier += c;
                    else if (!identifier.empty() && identifier.back() != '_') identifier += '_';
                }
                while (!identifier.empty() && identifier.back() == '_') identifier.pop_back();
                if (identifier.empty() || std::isdigit(static_cast<unsigned char>(identifier[0])))
                    identifier = std::string(fallback) + identifier;
                identifier[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(identifier[0])));

                // A component called "Switch" lowercases to a keyword, and a generated file that
                // does not compile is worse than one that is ugly.
                if (IsKeyword(identifier)) identifier += '_';

                const int seen = ++used[identifier];
                return seen == 1 ? identifier : identifier + std::to_string(seen);
            }

            const std::string& Of(Uuid id) const {
                static const std::string none = "Uuid::Invalid()";
                const auto it = byId.find(id.Value());
                return it == byId.end() ? none : it->second;
            }
        };

        std::string Float(f32 value) {
            std::ostringstream out;
            out.precision(9);
            out << value;
            std::string text = out.str();
            if (text.find('.') == std::string::npos && text.find('e') == std::string::npos)
                text += ".0";
            return text + "f";
        }

        std::string Quote(std::string_view text) {
            std::string out = "\"";
            for (const char c : text) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\t': out += "\\t";  break;
                    case '\r': out += "\\r";  break;
                    default:   out += c;      break;
                }
            }
            return out + "\"";
        }

        std::string Literal(const doc::Value& value) {
            switch (doc::TypeOf(value)) {
                case doc::ValueType::Bool:   return std::get<bool>(value) ? "true" : "false";
                case doc::ValueType::Number: return Float(std::get<f32>(value));
                case doc::ValueType::Vector2: {
                    const Vec2 v = std::get<Vec2>(value);
                    return "Vec2{ " + Float(v.x) + ", " + Float(v.y) + " }";
                }
                case doc::ValueType::Colour: {
                    const Color c = std::get<Color>(value);
                    return "Color{ " + Float(c.r) + ", " + Float(c.g) + ", " + Float(c.b) + ", "
                         + Float(c.a) + " }";
                }
                case doc::ValueType::Text:
                    return "std::string(" + Quote(std::get<std::string>(value)) + ")";
                case doc::ValueType::Token:
                    return "doc::TokenRef{ " + Quote(std::get<doc::TokenRef>(value).name) + " }";
                case doc::ValueType::Bound:
                    return "doc::Binding{ " + Quote(std::get<doc::Binding>(value).expression) + " }";
                case doc::ValueType::Asset:
                    // By id, and the table that gives those ids a file is emitted above. Dropping
                    // the id here is what made an exported app lose every picture in it.
                    return "doc::AssetRef{ Uuid(" + std::to_string(
                               std::get<doc::AssetRef>(value).id.Value()) + "ULL) }";
                default:
                    return "doc::Value{}";
            }
        }

        // `doc::Prop::CornerRadius` from `"cornerRadius"`. The serialized name is the enumerator
        // with a lowered first letter, so capitalising recovers it — a coupling the "emitted code
        // compiles" test exists to catch the moment it stops being true.
        std::string PropExpr(doc::Prop prop) {
            std::string name = doc::PropName(prop);
            if (!name.empty())
                name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            return "doc::Prop::" + name;
        }

        const char* KindExpr(doc::NodeKind kind) {
            switch (kind) {
                case doc::NodeKind::Text:      return "doc::NodeKind::Text";
                case doc::NodeKind::Image:     return "doc::NodeKind::Image";
                case doc::NodeKind::Vector:    return "doc::NodeKind::Vector";
                case doc::NodeKind::Component: return "doc::NodeKind::Component";
                default:                       return "doc::NodeKind::Frame";
            }
        }

        const char* ModeExpr(layout::LayoutMode mode) {
            return mode == layout::LayoutMode::Stack ? "LayoutMode::Stack" : "LayoutMode::Absolute";
        }
        const char* AxisExpr(layout::Axis axis) {
            return axis == layout::Axis::Row ? "Axis::Row" : "Axis::Column";
        }
        const char* AlignExpr(layout::Align align) {
            switch (align) {
                case layout::Align::Center:  return "Align::Center";
                case layout::Align::End:     return "Align::End";
                case layout::Align::Stretch: return "Align::Stretch";
                default:                     return "Align::Start";
            }
        }
        const char* JustifyExpr(layout::Justify justify) {
            switch (justify) {
                case layout::Justify::Center:       return "Justify::Center";
                case layout::Justify::End:          return "Justify::End";
                case layout::Justify::SpaceBetween: return "Justify::SpaceBetween";
                case layout::Justify::SpaceAround:  return "Justify::SpaceAround";
                case layout::Justify::SpaceEvenly:  return "Justify::SpaceEvenly";
                default:                            return "Justify::Start";
            }
        }
        const char* ConstraintExpr(layout::Constraint constraint) {
            switch (constraint) {
                case layout::Constraint::End:      return "Constraint::End";
                case layout::Constraint::StartEnd: return "Constraint::StartEnd";
                case layout::Constraint::Center:   return "Constraint::Center";
                case layout::Constraint::Scale:    return "Constraint::Scale";
                default:                           return "Constraint::Start";
            }
        }
        std::string SizeExpr(layout::Size size) {
            switch (size.mode) {
                case layout::SizeMode::Fixed:   return "Size::Px(" + Float(size.value) + ")";
                case layout::SizeMode::Fill:    return "Size::Fill(" + Float(size.value) + ")";
                case layout::SizeMode::Percent: return "Size::Percent(" + Float(size.value) + ")";
                default:                        return "Size::Hug()";
            }
        }

        // Only what differs from a default-constructed style. A generated file that restates every
        // field of every node is unreadable, and unreadable generated code is the same as none.
        void EmitLayout(std::ostringstream& out, const std::string& target,
                        const layout::LayoutStyle& style, const char* indent) {
            const layout::LayoutStyle base{};
            if (style == base) return;

            out << indent << "{\n";
            out << indent << "    LayoutStyle style;\n";
            const std::string field = std::string(indent) + "    style.";
            if (style.mode != base.mode)         out << field << "mode = " << ModeExpr(style.mode) << ";\n";
            if (style.axis != base.axis)         out << field << "axis = " << AxisExpr(style.axis) << ";\n";
            if (style.width != base.width)       out << field << "width = " << SizeExpr(style.width) << ";\n";
            if (style.height != base.height)     out << field << "height = " << SizeExpr(style.height) << ";\n";
            if (!(style.padding == base.padding))
                out << field << "padding = Edges(" << Float(style.padding.left) << ", "
                    << Float(style.padding.top) << ", " << Float(style.padding.right) << ", "
                    << Float(style.padding.bottom) << ");\n";
            if (style.gap != base.gap)           out << field << "gap = " << Float(style.gap) << ";\n";
            if (style.align != base.align)       out << field << "align = " << AlignExpr(style.align) << ";\n";
            if (style.justify != base.justify)   out << field << "justify = " << JustifyExpr(style.justify) << ";\n";
            if (style.wrap != base.wrap)         out << field << "wrap = true;\n";
            if (!(style.minSize == base.minSize))
                out << field << "minSize = Vec2{ " << Float(style.minSize.x) << ", "
                    << Float(style.minSize.y) << " };\n";
            if (!(style.maxSize == base.maxSize))
                out << field << "maxSize = Vec2{ " << Float(style.maxSize.x) << ", "
                    << Float(style.maxSize.y) << " };\n";
            if (style.aspectRatio != base.aspectRatio)
                out << field << "aspectRatio = " << Float(style.aspectRatio) << ";\n";
            if (!(style.offsetStart == base.offsetStart))
                out << field << "offsetStart = Vec2{ " << Float(style.offsetStart.x) << ", "
                    << Float(style.offsetStart.y) << " };\n";
            if (!(style.offsetEnd == base.offsetEnd))
                out << field << "offsetEnd = Vec2{ " << Float(style.offsetEnd.x) << ", "
                    << Float(style.offsetEnd.y) << " };\n";
            if (style.constraintX != base.constraintX)
                out << field << "constraintX = " << ConstraintExpr(style.constraintX) << ";\n";
            if (style.constraintY != base.constraintY)
                out << field << "constraintY = " << ConstraintExpr(style.constraintY) << ";\n";
            out << indent << "    b.Layout(" << target << ", style);\n";
            out << indent << "}\n";
        }

        void EmitProps(std::ostringstream& out, const std::string& target, const doc::PropBag& props) {
            for (const auto& [prop, value] : props.Known()) {
                if (!doc::IsSet(value)) continue;
                if (doc::TypeOf(value) == doc::ValueType::Token) {
                    out << "    b.Token(" << target << ", " << PropExpr(prop)
                        << ", " << Quote(std::get<doc::TokenRef>(value).name) << ");\n";
                } else {
                    out << "    b.Set(" << target << ", " << PropExpr(prop)
                        << ", " << Literal(value) << ");\n";
                }
            }
            for (const auto& [key, value] : props.Custom())
                out << "    b.Set(" << target << ", std::string(" << Quote(key) << "), "
                    << Literal(value) << ");\n";
        }

        // One node and everything under it. Components come out before the screens that instance
        // them, which is not an ordering choice — an instance needs the component's id to exist.
        void EmitSubtree(std::ostringstream& out, const doc::Document& document, Uuid id,
                         const std::string& parent, Names& names, bool detached) {
            const doc::Node* node = document.Find(id);
            if (!node) return;

            const std::string name = names.Take(node->name, "node");
            names.byId[id.Value()] = name;

            if (node->IsInstance()) {
                out << "    const Uuid " << name << " = b.Instance(" << names.Of(node->componentId)
                    << ", " << parent << ", " << Quote(node->name) << ");\n";
            } else if (detached) {
                // A component root is a Frame until it is sealed. Emitting it as a Component and
                // then sealing it would be asking the builder to convert what is already converted.
                const doc::NodeKind kind = node->kind == doc::NodeKind::Component
                                         ? doc::NodeKind::Frame : node->kind;
                out << "    const Uuid " << name << " = b.Detached(" << KindExpr(kind)
                    << ", " << Quote(node->name) << ");\n";
            } else {
                out << "    const Uuid " << name << " = b.Child(" << KindExpr(node->kind)
                    << ", " << parent << ", " << Quote(node->name) << ");\n";
            }

            EmitProps(out, name, node->props);
            EmitLayout(out, name, node->layout, "   ");
            if (!node->visible) out << "    b.Hide(" << name << ");\n";

            for (const auto& [inComponent, bag] : node->overrides)
                for (const auto& [prop, value] : bag.Known()) {
                    if (!doc::IsSet(value)) continue;
                    out << "    b.Override(" << name << ", " << names.Of(inComponent)
                        << ", " << PropExpr(prop) << ", " << Literal(value) << ");\n";
                }

            for (const Uuid child : node->children)
                EmitSubtree(out, document, child, name, names, false);
        }

    }

    std::string EmitDocument(const doc::Document& document, const Options& options) {
        std::ostringstream out;
        Names names;

        out << "// Generated by VAE. Edit it if you like — it is ordinary C++ against the same\n"
               "// public builder API a hand-written app would use, and nothing here is magic.\n"
               "//\n"
               "// Re-exporting overwrites this file.\n\n"
               "#include <vae/doc/Builder.h>\n\n"
               "#include <string>\n\n"
               "using namespace vae;\n"
               "using namespace vae::layout;\n\n"
            << "void " << options.function << "(doc::Document& document) {\n"
            << "    doc::Builder b(document);\n";

        // Tokens first: every colour in a well-built document is a reference to one, and a node
        // that resolves a token that does not exist yet renders as nothing.
        if (!document.Tokens().empty()) {
            if (options.comments) out << "\n    // Design tokens.\n";
            for (const auto& [name, token] : document.Tokens()) {
                out << "    b.DefineToken(" << Quote(name) << ", { " << Literal(token.light)
                    << ", " << Literal(token.dark);
                if (!token.description.empty()) out << ", " << Quote(token.description);
                out << " });\n";
            }
        }
        if (document.ActiveTheme() != doc::Theme::Dark)
            out << "    b.UseTheme(doc::Theme::Light);\n";

        // The asset table, with the ids the nodes refer to. EmitProject copies the files to the
        // same relative paths, so an exported app finds its pictures beside its binary.
        if (!document.Assets().empty()) {
            if (options.comments) out << "\n    // Assets, by the ids the nodes refer to.\n";
            for (const doc::Document::Asset& asset : document.Assets())
                out << "    document.AddAsset(" << Quote(asset.name) << ", " << Quote(asset.path)
                    << ", Uuid(" << asset.id.Value() << "ULL));\n";
        }

        // Components, then screens. A component is built detached and sealed, exactly as the
        // designer builds one: draw it, then say "this is a component".
        std::vector<Uuid> components, screens;
        for (const Uuid root : document.Roots()) {
            const doc::Node* node = document.Find(root);
            if (!node) continue;
            if (node->kind == doc::NodeKind::Component) components.push_back(root);
            else if (node->kind == doc::NodeKind::Screen) screens.push_back(root);
        }

        for (const Uuid component : components) {
            const doc::Node* node = document.Find(component);
            if (options.comments) out << "\n    // Component: " << node->name << "\n";

            std::ostringstream body;
            Names inner;
            inner.byId = names.byId;
            inner.used = names.used;
            EmitSubtree(body, document, component, "Uuid::Invalid()", inner, true);
            out << body.str();

            const std::string& root = inner.Of(component);
            out << "    b.Seal(" << root << ", " << Quote(node->name) << ");\n";
            names.byId = inner.byId;
            names.used = inner.used;
        }

        for (const Uuid screen : screens) {
            const doc::Node* node = document.Find(screen);
            if (options.comments) out << "\n    // Screen: " << node->name << "\n";

            const std::string name = names.Take(node->name, "screen");
            names.byId[screen.Value()] = name;
            out << "    const Uuid " << name << " = b.Screen(" << Quote(node->name) << ", Vec2{ "
                << Float(node->layout.width.value) << ", " << Float(node->layout.height.value)
                << " });\n";
            EmitProps(out, name, node->props);
            EmitLayout(out, name, node->layout, "   ");
            for (const Uuid child : node->children)
                EmitSubtree(out, document, child, name, names, false);
        }

        // Last, because it names a screen and every screen has to exist by then.
        if (document.StartScreen().Valid() && screens.size() > 1) {
            const doc::Node* start = document.Find(document.StartScreen());
            if (start && options.comments)
                out << "\n    // The screen the app opens on.\n";
            if (start)
                out << "    b.Doc().SetStartScreen(" << names.Of(document.StartScreen()) << ");\n";
        }

        out << "}\n";
        return out.str();
    }

    namespace {

        std::string MainSource(const Options& options) {
            return "// Generated by VAE. The app: build the document, run it.\n"
                   "\n"
                   "#include <vae/app/RunLayer.h>\n"
                   "#include <vae/core/Application.h>\n"
                   "#include <vae/core/EntryPoint.h>\n"
                   "#include <vae/text/FontDB.h>\n"
                   "\n"
                   "void " + options.function + "(vae::doc::Document& document);\n"
                   "\n"
                   "namespace vae {\n"
                   "\n"
                   "    Application* CreateApplication(CommandLineArgs args) {\n"
                   "        auto layer = CreateScope<app::RunLayer>();\n"
                   "        " + options.function + "(layer->Doc());\n"
                 + (options.script.empty()
                        ? std::string{}
                        : "        layer->SetScript(\"" + options.script + "\");\n")
                 + "        layer->Start();\n"
                   "\n"
                   "        AppSpec spec;\n"
                   "        spec.name           = \"" + options.appName + "\";\n"
                   "        spec.args           = args;\n"
                   "        spec.window.title   = \"" + options.appName + "\";\n"
                   "        spec.window.wmClass = \"VAE\";\n"
                   "        spec.enableImGui    = false;\n"
                   "        spec.window.width   = static_cast<u32>(layer->DesignSize().x);\n"
                   "        spec.window.height  = static_cast<u32>(layer->DesignSize().y);\n"
                   "\n"
                   "        // The fonts the design used, carried beside the binary. Registered\n"
                   "        // before the system ones so the app looks the same on a machine that\n"
                   "        // does not have them installed.\n"
                   "        text::FontDB::Get().RegisterDirectory(\"fonts\", false, true);\n"
                   "\n"
                   "        auto* app = new Application(std::move(spec));\n"
                   "        app->PushLayer(std::move(layer));\n"
                   "        return app;\n"
                   "    }\n"
                   "\n"
                   "}\n";
        }

        std::string PremakeSource(const Options& options, const std::filesystem::path& engine) {
            return "-- Generated by VAE. Builds this app against the engine it was exported from.\n"
                   "--\n"
                   "-- Run `premake5 gmake && make` here, or copy this project into a workspace of\n"
                   "-- your own — it is an ordinary premake project with no VAE-specific magic.\n"
                   "\n"
                   "VAE_ROOT = \"" + engine.generic_string() + "\"\n"
                   "\n"
                   "workspace \"" + options.appName + "\"\n"
                   "   configurations { \"Debug\", \"Release\" }\n"
                   "   architecture \"x86_64\"\n"
                   "   startproject \"" + options.appName + "\"\n"
                   "\n"
                   "outputdir = \"%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}\"\n"
                   "\n"
                   "project \"" + options.appName + "\"\n"
                   "   kind \"ConsoleApp\"\n"
                   "   language \"C++\"\n"
                   "   cppdialect \"C++23\"\n"
                   "   targetdir (\"bin/\" .. outputdir)\n"
                   "   objdir    (\"bin-int/\" .. outputdir)\n"
                   "\n"
                   "   files { \"*.cpp\" }\n"
                   "\n"
                 + (options.script.empty()
                        ? std::string{}
                        : "   -- The script lives beside the binary, so the exported folder can be\n"
                          "   -- moved anywhere and still run.\n"
                          "   postbuildcommands { \"{COPYFILE} %{prj.location}/" + options.script
                              + " %{cfg.targetdir}\" }\n"
                          "\n") + ""
                   "   includedirs {\n"
                   "      VAE_ROOT .. \"/VAE/src\",\n"
                   "      VAE_ROOT .. \"/VAE/vendor/spdlog/include\",\n"
                   "      VAE_ROOT .. \"/VAE/vendor/glm\",\n"
                   "      VAE_ROOT .. \"/VAE/vendor/json/include\",\n"
                   "      VAE_ROOT .. \"/VAE/vendor/Vulkan-Headers/include\",\n"
                   "   }\n"
                   "\n"
                   "   defines { \"SPDLOG_COMPILED_LIB\", \"GLFW_INCLUDE_NONE\",\n"
                   "             \"GLM_FORCE_DEPTH_ZERO_TO_ONE\", \"GLM_ENABLE_EXPERIMENTAL\" }\n"
                   "\n"
                   "   -- StaticLib links are NOT transitive under gmake, so every dependency is listed.\n"
                   "   -- The engine's own output folder is named after the system it was built on, so\n"
                   "   -- this has to be spelled the same way premake spells it rather than hardcoded.\n"
                   "   libdirs { VAE_ROOT .. \"/bin/\" .. outputdir .. \"/*\" }\n"
                   // Every static library the engine itself links, because a StaticLib link is not
                   // transitive under gmake and a missing one is an undefined reference at the very
                   // last step of somebody else's build. Kept in step with VAE-Player's list, which
                   // is the same thing this is: the engine with no editor around it.
                   // No ImGui: the editor chrome is installed through a factory the editor names,
                   // so a shipped app does not reference it and must not link it either. That is
                   // 1.4 MB of toolkit an exported app used to carry and could never open.
                   "   links { \"VAE\", \"VAE-Core\", \"spdlog\", \"GLFW\", \"VulkanDeps\",\n"
                   "           \"Lua\", \"miniaudio\", \"pugixml\" }\n"
                   "\n"
                   "   filter \"system:linux\"\n"
                   "      defines { \"VAE_PLATFORM_LINUX\" }\n"
                   "      links { \"dl\", \"pthread\", \"X11\", \"vulkan\" }\n"
                   // Whatever the engine was built with, the app has to be linked with: httplib is
                   // header-only, so the TLS calls are already inlined into libVAE.a and leaving
                   // these out is an undefined reference to ERR_get_error at the very last step.
#ifdef VAE_HTTP_TLS
                   "      links { \"ssl\", \"crypto\" }\n"
#endif
                   "\n"
                   "   filter \"system:windows\"\n"
                   "      systemversion \"latest\"\n"
                   "      defines { \"VAE_PLATFORM_WINDOWS\", \"NOMINMAX\", \"WIN32_LEAN_AND_MEAN\" }\n"
                   "      buildoptions { \"/utf-8\" }\n"
                   "      libdirs { (os.getenv(\"VULKAN_SDK\") or \"\") .. \"/Lib\" }\n"
                   "      links { \"vulkan-1\", \"gdi32\", \"ws2_32\", \"shell32\", \"ole32\" }\n"
                   "\n"
                   "   filter {}\n"
                   "\n"
                   "   filter \"configurations:Debug\"\n"
                   "      defines { \"VAE_DEBUG\" }\n"
                   "      symbols \"on\"\n"
                   "\n"
                   "   filter \"configurations:Release\"\n"
                   "      defines { \"VAE_RELEASE\" }\n"
                   "      optimize \"on\"\n";
        }

    }

    bool EmitProject(const doc::Document& document, const std::filesystem::path& directory,
                     const Options& options, std::string* error) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            if (error) *error = "could not create " + directory.string() + ": " + ec.message();
            return false;
        }

        const std::filesystem::path source = directory / "Document.cpp";
        if (!FileSystem::WriteText(source, EmitDocument(document, options))) {
            if (error) *error = "could not write " + source.string();
            return false;
        }
        if (!FileSystem::WriteText(directory / "Main.cpp", MainSource(options))) {
            if (error) *error = "could not write Main.cpp";
            return false;
        }
        if (!FileSystem::WriteText(directory / "premake5.lua",
                                   PremakeSource(options, FileSystem::EngineRoot()))) {
            if (error) *error = "could not write premake5.lua";
            return false;
        }

        // The pictures come too, at the same relative paths, so the emitted document's asset ids
        // resolve beside the binary exactly as they did beside the project.
        u32 copied = 0;
        for (const doc::Document::Asset& asset : document.Assets()) {
            if (asset.path.empty() || options.assetRoot.empty()) continue;
            const std::filesystem::path from = options.assetRoot / asset.path;
            const std::filesystem::path to = directory / asset.path;
            std::filesystem::create_directories(to.parent_path(), ec);
            std::filesystem::copy_file(from, to,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) VAE_WARN("export: could not copy {}: {}", asset.path, ec.message());
            else ++copied;
            ec.clear();
        }

        // The fonts the document actually names, beside the binary. An exported app that relies
        // on the designer's machine having their fonts installed is an app that renders differently
        // everywhere else — and the fallback is silent, so nobody notices until a screenshot.
        //
        // Only what the document names, never the whole system: this is copying somebody's font
        // files, and the licence for that is the exporter's to check, not ours to assume.
        u32 fonts = 0;
        {
            std::set<std::string> families;
            for (Uuid id : document.AllNodes()) {
                const doc::Node* node = document.Find(id);
                if (!node) continue;
                const std::string family = node->props.Text(doc::Prop::FontFamily);
                if (!family.empty()) families.insert(family);
            }
            families.insert(text::FontDB::Get().DefaultFamily());

            for (const std::string& family : families) {
                for (const text::FontFaceInfo& face : text::FontDB::Get().Faces(family)) {
                    if (face.path.empty()) continue;
                    const std::filesystem::path to = directory / "fonts" / face.path.filename();
                    std::filesystem::create_directories(to.parent_path(), ec);
                    std::filesystem::copy_file(face.path, to,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) ++fonts;
                    ec.clear();
                }
            }
        }

        // Translations travel with it too, and the runtime looks for them beside the binary when
        // the document was built in code rather than loaded from a file.
        u32 locales = 0;
        if (!options.assetRoot.empty()) {
            const std::filesystem::path from = options.assetRoot / "strings";
            if (std::filesystem::is_directory(from, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
                    if (entry.path().extension() != ".json") continue;
                    const std::filesystem::path to = directory / "strings" / entry.path().filename();
                    std::filesystem::create_directories(to.parent_path(), ec);
                    std::filesystem::copy_file(entry.path(), to,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) ++locales;
                    ec.clear();
                }
            }
            ec.clear();
        }
        if (locales) VAE_INFO("export: {} translation file(s)", locales);

        VAE_INFO("export: wrote {} to {}{}{}", options.appName, directory.string(),
                 copied ? " (" + std::to_string(copied) + " assets)" : "",
                 fonts ? " (" + std::to_string(fonts) + " font files — check their licences before "
                                "shipping)" : "");
        return true;
    }

}
