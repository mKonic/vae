#include "vae/app/RunLayer.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/base/Version.h"
#include "vae/core/Application.h"
#include "vae/core/EntryPoint.h"

#include <cstring>
#include <filesystem>
#include <iostream>

namespace vae {

    namespace {

        struct Options {
            std::filesystem::path project;
            std::string screen;
            bool headless = false;
            int  frames = 1;
            bool help = false;
            bool version = false;
            std::string locale;
        };

        Options Parse(CommandLineArgs args) {
            Options options;
            for (int i = 1; i < args.count; ++i) {
                const std::string_view arg = args.values[i];
                const auto next = [&]() -> std::string {
                    return i + 1 < args.count ? args.values[++i] : std::string{};
                };
                if (arg == "--screen")        options.screen = next();
                else if (arg == "--headless") options.headless = true;
                else if (arg == "--frames")   options.frames = std::max(1, std::atoi(next().c_str()));
                else if (arg == "--help" || arg == "-h") options.help = true;
                else if (arg == "--version") options.version = true;
                else if (arg == "--locale") options.locale = next();
                else if (!arg.starts_with("-")) options.project = arg;
            }
            return options;
        }

        void Usage() {
            std::cout << "usage: VAE-Player <project folder | file.vae> [--screen NAME] [--locale NAME]\n"
                         "                  [--headless [--frames N]]\n"
                         "       VAE-Player --version\n"
                         "\n"
                         "  Runs a project. The script beside it — <project>.so or <project>.lua —\n"
                         "  is loaded automatically; a native module wins when both are present.\n"
                         "\n"
                         "  --headless  lay out and paint without a window, then report what was\n"
                         "              drawn. Exits non-zero if the project or its script failed.\n";
        }

    }

    // The whole app, with no editor around it: a window, a document, and the script that drives it.
    class PlayerApp final : public Application {
    public:
        PlayerApp(AppSpec spec, Scope<app::RunLayer> layer) : Application(std::move(spec)) {
            PushLayer(std::move(layer));
        }
    };

    Application* CreateApplication(CommandLineArgs args) {
        const Options options = Parse(args);

        AppSpec spec;
        spec.name           = "VAE Player";
        spec.args           = args;
        spec.window.title   = "VAE Player";
        spec.window.wmClass = "VAE";
        spec.enableImGui    = false;

        if (options.version) {
            std::cout << "VAE Player " << Version::String() << "\n";
            spec.createWindow = false;
            spec.createDevice = false;
            auto* app = new PlayerApp(std::move(spec), CreateScope<app::RunLayer>());
            app->Close();
            return app;
        }

        if (options.help || options.project.empty()) {
            Usage();
            spec.createWindow = false;
            spec.createDevice = false;
            auto* app = new PlayerApp(std::move(spec), CreateScope<app::RunLayer>());
            app->SetExitCode(options.help ? 0 : 2);
            app->Close();
            return app;
        }

        auto layer = CreateScope<app::RunLayer>();
        layer->SetStartScreen(options.screen);
        // Explicit beats the environment; empty means "whatever this machine is set to".
        layer->SetLocale(options.locale);

        std::string error;
        const bool loaded = layer->Load(options.project, &error);
        if (!loaded) VAE_ERROR("player: {}", error);

        // Headless still walks the whole path — document, layout, scripts, paint — because the
        // point of it is to answer "would this run?" in a test, not to skip the interesting half.
        if (options.headless || !loaded) {
            spec.createWindow = false;
            spec.createDevice = false;
            app::RunLayer* raw = layer.get();
            auto* app = new PlayerApp(std::move(spec), std::move(layer));
            if (loaded) {
                for (int i = 0; i < options.frames; ++i) raw->RenderOffline(1.0f / 60.0f);
                const draw::DrawList& list = raw->RenderOffline(1.0f / 60.0f);
                VAE_INFO("player: headless — {} quads + {} shadows in {} batch(es), {} live script(s)",
                         list.Quads().size(), list.Shadows().size(), list.Batches().size(),
                         raw->Runtime().LiveCount());
                app->SetExitCode(raw->ScriptError().empty() ? 0 : 1);
            } else {
                app->SetExitCode(2);
            }
            app->Close();
            return app;
        }

        spec.window.width  = static_cast<u32>(layer->DesignSize().x);
        spec.window.height = static_cast<u32>(layer->DesignSize().y);
        // A screen pinned to a resolution gets a window that cannot be dragged. Asked of the
        // window manager at creation, because there is no honest way to refuse a resize afterwards
        // — the frame would fight the user's mouse.
        spec.window.resizable = layer->Resizable();
        return new PlayerApp(std::move(spec), std::move(layer));
    }

}
