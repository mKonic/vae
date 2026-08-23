#include "Test.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/base/Platform.h"
#include "vae/doc/Builder.h"
#include "vae/svc/Services.h"
#include "vae/doc/Serializer.h"
#include "vae/gen/Emit.h"
#include "vae/text/FontDB.h"
#include "vae/script/NativeHost.h"
#include "vae/ui/Library.h"
#include "vae/ui/UiHost.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace vae;

namespace {

    namespace fs = std::filesystem;

    fs::path Scratch() {
        const fs::path dir = fs::temp_directory_path() / "vae-gen-tests";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    // A document with one of everything the emitter has to say something about: tokens, a component
    // sealed from a detached subtree, an instance with overrides, stack and absolute layout, text,
    // a hidden node, and a custom property.
    void BuildSample(doc::Document& document) {
        doc::Builder b(document);

        b.DefineToken("surface", { Color{ 0.96f, 0.96f, 0.97f, 1.0f },
                                   Color{ 0.10f, 0.11f, 0.13f, 1.0f }, "page background" });
        b.DefineToken("accent",  { Color{ 0.24f, 0.42f, 0.92f, 1.0f },
                                   Color{ 0.42f, 0.55f, 0.96f, 1.0f } });

        const Uuid card = b.Detached(doc::NodeKind::Frame, "Card");
        {
            layout::LayoutStyle style;
            style.mode = layout::LayoutMode::Stack;
            style.axis = layout::Axis::Column;
            style.gap = 12.0f;
            style.padding = Edges(20.0f, 16.0f);
            style.align = layout::Align::Center;
            style.width = layout::Size::Px(240.0f);
            b.Layout(card, style);
        }
        b.Token(card, doc::Prop::Fill, "surface");
        b.Set(card, doc::Prop::CornerRadius, 10.0f);

        const Uuid title = b.Text(card, "Title", "Card");
        b.Set(title, doc::Prop::FontSize, 15.0f);
        b.Token(title, doc::Prop::TextColor, "accent");

        const Uuid body = b.Text(card, "Body", "Some words");
        b.Set(body, doc::Prop::TextWrap, std::string("word"));

        const Uuid hidden = b.Frame(card, "Hidden");
        b.Hide(hidden);

        // A state overlay: a custom, string-keyed property, which is how the widget library says
        // "this colour, but only when hovered". The emitter has to round-trip these too.
        b.Set(card, std::string("hovered:fill"), doc::TokenRef{ "accent" });

        const Uuid component = b.Seal(card, "Card");

        const Uuid home = b.Screen("Home", { 640.0f, 480.0f });
        b.Token(home, doc::Prop::Fill, "surface");

        const Uuid one = b.Instance(component, home, "First");
        {
            layout::LayoutStyle style = document.Find(one)->layout;
            style.offsetStart = { 24.0f, 24.0f };
            b.Layout(one, style);
        }
        b.Override(one, title, doc::Prop::Text, std::string("Overridden"));

        const Uuid two = b.Instance(component, home, "Second");
        {
            layout::LayoutStyle style = document.Find(two)->layout;
            style.offsetStart = { 320.0f, 24.0f };
            style.constraintX = layout::Constraint::End;
            b.Layout(two, style);
        }

        // Screens, their kinds, and the relations between them. An export that loses these opens on
        // whichever screen happens to be first and has buttons that go nowhere.
        const Uuid go = b.Frame(home, "Go");
        b.Set(go, doc::Prop::GoTo, std::string("Detail"));

        const Uuid detail = b.Screen("Detail", { 640.0f, 480.0f });
        b.Token(detail, doc::Prop::Fill, "surface");

        const Uuid confirm = b.Screen("Confirm", { 320.0f, 180.0f });
        b.Set(confirm, doc::Prop::ScreenKind, std::string("alert"));

        b.Doc().SetStartScreen(home);
    }

    // Two documents draw the same app when they emit the same primitives. Pixels would say the same
    // thing and need a GPU to say it; these ARE the pixels' input, byte for byte.
    struct Frame {
        std::vector<draw::QuadInstance> quads;
        std::vector<draw::ShadowInstance> shadows;
        std::size_t batches = 0;
    };

    Frame Draw(doc::Document& document) {
        Uuid screen = Uuid::Invalid();
        for (const Uuid root : document.Roots())
            if (const doc::Node* node = document.Find(root); node
                && node->kind == doc::NodeKind::Screen) { screen = root; break; }

        ui::UiHost host;
        host.SetDocument(document, screen);
        host.Update({ 640.0f, 480.0f }, 1.0f / 60.0f);

        draw::DrawList list;
        ui::PaintContext paint;
        paint.list = &list;
        paint.atlas = nullptr;          // no device, so no glyph quads — geometry is the question
        host.Paint(paint);

        return { list.Quads(), list.Shadows(), list.Batches().size() };
    }

    bool Same(const Frame& a, const Frame& b) {
        if (a.quads.size() != b.quads.size() || a.shadows.size() != b.shadows.size()) return false;
        if (a.batches != b.batches) return false;
        for (std::size_t i = 0; i < a.quads.size(); ++i)
            if (std::memcmp(&a.quads[i], &b.quads[i], sizeof(draw::QuadInstance)) != 0) return false;
        for (std::size_t i = 0; i < a.shadows.size(); ++i)
            if (std::memcmp(&a.shadows[i], &b.shadows[i], sizeof(draw::ShadowInstance)) != 0)
                return false;
        return true;
    }

}

TEST(gen, emitted_code_says_what_the_document_says) {
    doc::Document document;
    BuildSample(document);
    const std::string source = gen::EmitDocument(document);

    // Not a golden byte-comparison: the point is that the emitted text is readable code naming the
    // things the designer named, and a golden file would fail on every whitespace change instead.
    CHECK(source.find("#include <vae/doc/Builder.h>") != std::string::npos);
    CHECK(source.find("void BuildDocument(doc::Document& document)") != std::string::npos);
    CHECK(source.find("b.DefineToken(\"surface\"") != std::string::npos);
    CHECK(source.find("b.Detached(doc::NodeKind::Frame, \"Card\")") != std::string::npos);
    CHECK(source.find("b.Seal(") != std::string::npos);
    CHECK(source.find("b.Screen(\"Home\", Vec2{ 640.0f, 480.0f })") != std::string::npos);
    CHECK(source.find("b.Instance(") != std::string::npos);
    CHECK(source.find("b.Override(") != std::string::npos);
    CHECK(source.find("b.Hide(") != std::string::npos);
    CHECK(source.find("doc::Prop::Fill, \"surface\")") != std::string::npos);
    CHECK(source.find("// Component: Card") != std::string::npos);
    CHECK(source.find("// Screen: Home") != std::string::npos);
    // The overridden text has to survive, or two instances of one component are indistinguishable.
    CHECK(source.find("\"Overridden\"") != std::string::npos);
    // Constraints and enums come out as names, not as the integers they are stored as.
    CHECK(source.find("Constraint::End") != std::string::npos);
    CHECK(source.find("Align::Center") != std::string::npos);
    CHECK(source.find("std::string(\"hovered:fill\")") != std::string::npos);
    // Navigation is document data, so it comes out with everything else.
    CHECK(source.find("doc::Prop::GoTo, std::string(\"Detail\")") != std::string::npos);
    CHECK(source.find("doc::Prop::ScreenKind, std::string(\"alert\")") != std::string::npos);
    CHECK(source.find("SetStartScreen(") != std::string::npos);
}

TEST(gen, the_emitted_code_compiles_and_rebuilds_the_same_document) {
    doc::Document original;
    BuildSample(original);

    // The emitted builder, plus a shim that runs it and reports what it built. Compiled with the
    // engine's own headers, which is the only way to know the emitted code is really C++ and not
    // C++-shaped text.
    const fs::path dir = Scratch();
    const fs::path source = dir / "emitted.cpp";
    {
        std::ofstream out(source, std::ios::trunc);
        out << gen::EmitDocument(original);
        out << R"(
#ifdef _MSC_VER
    #define VAE_GEN_TEST_EXPORT extern "C" __declspec(dllexport)
#else
    #define VAE_GEN_TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif
VAE_GEN_TEST_EXPORT void vae_gen_test_build(void* document) {
    BuildDocument(*static_cast<vae::doc::Document*>(document));
}
)";
    }

    const fs::path module = dir / (std::string("emitted") + platform::ModuleExtension());
    std::string diagnostics;
    const bool built = script::NativeHost::Compile(source, module, &diagnostics);
    CHECK_MESSAGE(built, diagnostics);
    if (!built) return;

    // Loaded rather than linked, so a failure to compile is reported as a compiler error with line
    // numbers instead of taking the whole test binary with it.
    platform::Module handle = platform::LoadModule(fs::absolute(module));
    CHECK_MESSAGE(handle != nullptr, platform::ModuleError());
    if (!handle) return;

    using BuildFn = void (*)(void*);
    auto build = reinterpret_cast<BuildFn>(platform::ModuleSymbol(handle, "vae_gen_test_build"));
    CHECK(build != nullptr);
    if (!build) { platform::FreeModule(handle); return; }

    doc::Document rebuilt;
    build(&rebuilt);

    CHECK(rebuilt.NodeCount() == original.NodeCount());
    CHECK(rebuilt.Roots().size() == original.Roots().size());
    CHECK(rebuilt.Tokens().size() == original.Tokens().size());

    // The exported app navigates: it opens where the designer said, its alert is presented over
    // the screen rather than replacing it, and Back unwinds.
    {
        ui::UiHost host;
        host.SetDocument(rebuilt, rebuilt.StartScreen());
        host.Update({ 640.0f, 480.0f }, 1.0f / 60.0f);
        CHECK(host.CurrentScreenName() == "Home");

        CHECK(host.GoToScreen("Detail"));
        host.Update({ 640.0f, 480.0f }, 1.0f / 60.0f);
        CHECK(host.CurrentScreenName() == "Detail");

        CHECK(host.GoToScreen("Confirm"));
        host.Update({ 640.0f, 480.0f }, 1.0f / 60.0f);
        CHECK(host.CurrentScreenName() == "Detail");
        CHECK(host.OverlayCount() == 1);

        CHECK(host.GoBack());
        CHECK(host.OverlayCount() == 0);
        CHECK(host.GoBack());
        host.Update({ 640.0f, 480.0f }, 1.0f / 60.0f);
        CHECK(host.CurrentScreenName() == "Home");
    }

    // The real question: does it draw the same app? Ids differ — they are generated fresh — so the
    // comparison is what came out the far end, which is the only thing a user can see.
    const Frame before = Draw(original);
    const Frame after  = Draw(rebuilt);
    CHECK_MESSAGE(!before.quads.empty(), "the sample draws something at all");
    CHECK_MESSAGE(Same(before, after),
                  "generated: " + std::to_string(after.quads.size()) + " quads in "
                      + std::to_string(after.batches) + " batch(es); loaded: "
                      + std::to_string(before.quads.size()) + " quads in "
                      + std::to_string(before.batches));

    platform::FreeModule(handle);
}

TEST(gen, a_round_trip_through_the_serializer_and_the_emitter_agree) {
    doc::Document original;
    BuildSample(original);

    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromJson(doc::Serializer::ToJson(original, false), loaded, &error),
                  error);

    // Two paths out of one document — save/load and export/compile — have to arrive at the same
    // picture, or one of them is losing something the other keeps.
    CHECK(Same(Draw(original), Draw(loaded)));
    CHECK(gen::EmitDocument(loaded).size() > 0);
}

TEST(gen, exporting_a_project_writes_something_that_builds) {
    doc::Document document;
    BuildSample(document);

    const fs::path dir = Scratch() / "project";
    std::error_code ec;
    fs::remove_all(dir, ec);

    // What the running editor has loaded is what the exporter copies, so the test has to have it
    // loaded too.
    text::FontDB::Get().RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    text::FontDB::Get().SetDefaultFamily("JetBrains Mono Nerd Font");

    gen::Options options;
    options.appName = "Sample";
    std::string error;
    CHECK_MESSAGE(gen::EmitProject(document, dir, options, &error), error);

    CHECK(fs::exists(dir / "Document.cpp"));
    CHECK(fs::exists(dir / "Main.cpp"));
    CHECK(fs::exists(dir / "premake5.lua"));

    // The fonts travel with it: an exported app that renders in whatever the target machine
    // happens to have installed is an app that looks different everywhere, silently.
    std::error_code fontEc;
    const bool anyFont = fs::exists(dir / "fonts", fontEc)
                      && fs::directory_iterator(dir / "fonts", fontEc) != fs::directory_iterator();
    CHECK(anyFont);
    const auto entry = FileSystem::ReadText(dir / "Main.cpp");
    CHECK(entry.has_value());
    if (entry) CHECK(entry->find("RegisterDirectory(\"fonts\"") != std::string::npos);

    // And it links. Checking the text of the generated premake is what let the export ship for
    // weeks without miniaudio or pugixml in its link list — the file said everything a reader
    // would look for and the linker said "undefined reference to ma_sound_uninit". The only thing
    // that can tell the difference is a linker.
    //
    // Skipped, loudly, where premake5 is not on PATH: a machine that cannot generate the project
    // cannot answer the question either way, and failing there would be reporting the wrong thing.
    if (const int probe = std::system("premake5 --version >/dev/null 2>&1"); probe != 0) {
        VAE_WARN("gen: premake5 not on PATH — the exported project was written but not linked");
    } else {
        const std::string generate = "cd '" + dir.string() + "' && premake5 gmake >/dev/null 2>&1";
        CHECK_EQ(std::system(generate.c_str()), 0);
        const std::string build = "cd '" + dir.string() + "' && make config=release -j4 "
                                  "> build.log 2>&1";
        const bool linked = std::system(build.c_str()) == 0;
        if (!linked) {
            const auto log = FileSystem::ReadText(dir / "build.log");
            CHECK_MESSAGE(linked, log ? log->substr(0, 2000) : std::string("no build log"));
        } else {
            CHECK(linked);
            CHECK(fs::exists(dir / "bin" / "Release-linux-x86_64" / "Sample"));
        }
    }

    const auto premake = FileSystem::ReadText(dir / "premake5.lua");
    CHECK(premake.has_value());
    if (premake) {
        CHECK(premake->find("project \"Sample\"") != std::string::npos);
        // Without the engine root the generated project cannot find a single header.
        CHECK(premake->find(FileSystem::EngineRoot().generic_string()) != std::string::npos);

        // The engine's library folder is named after the system it was built on. Spelling it out
        // exports a project that only links on the machine that made it.
        CHECK(premake->find("linux-x86_64") == std::string::npos);
        CHECK(premake->find("outputdir .. \"/*\"") != std::string::npos);

        // Both halves of the platform split are emitted, so the folder can be carried to a Windows
        // machine and generated there without editing.
        CHECK(premake->find("filter \"system:linux\"") != std::string::npos);
        CHECK(premake->find("filter \"system:windows\"") != std::string::npos);

        // Whatever the engine was built with, the app has to be linked with. httplib is header
        // only, so an engine built against OpenSSL has already inlined the TLS calls into its
        // archive and an app that does not link it fails at the very last step.
        CHECK((premake->find("\"ssl\", \"crypto\"") != std::string::npos) == svc::HasTls());
    }

    const auto main = FileSystem::ReadText(dir / "Main.cpp");
    CHECK(main.has_value());
    if (main) {
        CHECK(main->find("vae/app/RunLayer.h") != std::string::npos);
        CHECK(main->find("BuildDocument") != std::string::npos);
    }
}

TEST(gen, an_export_carries_its_assets) {
    doc::Document document;
    const Uuid logo = document.AddAsset("logo", "assets/logo.png");
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid picture = document.CreateNode(doc::NodeKind::Image, screen, "Picture");
    document.SetProp(picture, doc::Prop::Image, doc::AssetRef{ logo });

    const std::string source = gen::EmitDocument(document);
    // Both halves: the table that says where the file is, and the reference that points at it.
    // Emitting the reference as an empty AssetRef is what made an exported app lose its pictures.
    CHECK(source.find("AddAsset(\"logo\", \"assets/logo.png\"") != std::string::npos);
    CHECK(source.find("doc::AssetRef{ Uuid(" + std::to_string(logo.Value()) + "ULL) }")
          != std::string::npos);
}

TEST(gen, an_export_remembers_that_a_screen_is_a_fixed_resolution) {
    // A screen pinned to a resolution is the one case where the window must NOT follow the user's
    // mouse, and the only record of that is a flag on the screen. An export that dropped it would
    // produce an app that quietly became resizable again.
    doc::Document document;
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Kiosk");
    document.SetProp(screen, doc::Prop::Resizable, false);

    const std::string source = gen::EmitDocument(document);
    CHECK(source.find("doc::Prop::Resizable, false)") != std::string::npos);
}

TEST(gen, an_export_carries_a_sound_no_node_points_at) {
    // The case every asset pipeline gets wrong: a sound is referenced by *name from a script*, not
    // by a property on a node. Nothing in the document tree points at it, so a copier that walks
    // the tree instead of the asset table would leave it behind and the app would run silently.
    doc::Document document;
    document.AddAsset("click", "assets/click.wav");
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    document.CreateNode(doc::NodeKind::Text, screen, "Label");

    const fs::path dir = Scratch() / "sound-export";
    std::error_code ec;
    fs::remove_all(dir, ec);

    const fs::path assets = Scratch() / "sound-source";
    fs::create_directories(assets / "assets", ec);
    { std::ofstream out(assets / "assets" / "click.wav", std::ios::binary); out << "RIFF...."; }

    gen::Options options;
    options.appName = "Noisy";
    options.assetRoot = assets;
    std::string error;
    CHECK_MESSAGE(gen::EmitProject(document, dir, options, &error), error);

    CHECK(fs::exists(dir / "assets" / "click.wav"));
    const auto source = FileSystem::ReadText(dir / "Document.cpp");
    CHECK(source.has_value());
    if (source)
        CHECK(source->find("AddAsset(\"click\", \"assets/click.wav\"") != std::string::npos);
}

TEST(gen, an_export_carries_its_artwork_too) {
    // Artwork travels the same way a picture does — an id, a relative path and a node kind — and
    // the file that has to be copied beside the binary is the .svg, not a rasterization of it.
    doc::Document document;
    const Uuid mark = document.AddAsset("mark", "assets/mark.svg");
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    const Uuid icon = document.CreateNode(doc::NodeKind::Vector, screen, "Icon");
    document.SetProp(icon, doc::Prop::Image, doc::AssetRef{ mark });
    document.SetProp(icon, doc::Prop::Fill, doc::TokenRef{ "text" });

    const std::string source = gen::EmitDocument(document);
    CHECK(source.find("doc::NodeKind::Vector") != std::string::npos);
    CHECK(source.find("AddAsset(\"mark\", \"assets/mark.svg\"") != std::string::npos);
    CHECK(source.find("doc::AssetRef{ Uuid(" + std::to_string(mark.Value()) + "ULL) }")
          != std::string::npos);
    // The colour is a token, so the exported app recolours with the theme rather than being
    // stamped with whatever the palette happened to be at export time.
    CHECK(source.find("doc::Prop::Fill, \"text\")") != std::string::npos);
}
