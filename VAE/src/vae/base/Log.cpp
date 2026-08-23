#include "vaepch.h"
#include "vae/base/Log.h"

#include "vae/base/FileSystem.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cstdio>
#include <filesystem>
#include <spdlog/sinks/dist_sink.h>

namespace vae {

    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_AppLogger;
    std::shared_ptr<spdlog::sinks::sink> Log::s_File;

    namespace { std::shared_ptr<spdlog::sinks::dist_sink_mt> s_Dist; }

    void Log::Init() {
        if (s_CoreLogger) return;

        // A dist_sink lets Studio attach its Console sink after the loggers already exist,
        // instead of every logger having to be rebuilt when a panel opens.
        s_Dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        s_Dist->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        // And to a file, because a console is whatever was attached when it happened. A crash on
        // somebody else's machine leaves the last run behind either way: two files, rotated, so
        // the previous session survives the one that replaced it.
        try {
            const std::filesystem::path dir = FileSystem::ConfigRoot() / "logs";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            s_File = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (dir / "vae.log").string(), 2 * 1024 * 1024, 2, true);
            s_Dist->add_sink(s_File);
        } catch (const std::exception& e) {
            // A read-only home is not a reason to run without logging at all.
            std::fprintf(stderr, "log: no file sink (%s)\n", e.what());
        }

        s_CoreLogger = std::make_shared<spdlog::logger>("VAE", s_Dist);
        s_AppLogger  = std::make_shared<spdlog::logger>("APP", s_Dist);

        for (auto* l : { &s_CoreLogger, &s_AppLogger }) {
            (*l)->set_pattern("%^[%T] %n: %v%$");
            (*l)->set_level(spdlog::level::trace);
            (*l)->flush_on(spdlog::level::trace);
            spdlog::register_logger(*l);
        }

        // Last, because setting a pattern on a logger sets it on every sink the logger has. The
        // file wants the date and the level spelled out; the console wants to stay short.
        if (s_File) s_File->set_pattern("[%Y-%m-%d %T.%e] [%n] [%l] %v");
    }

    void Log::Shutdown() {
        spdlog::drop_all();
        s_File.reset();
        s_CoreLogger.reset();
        s_AppLogger.reset();
        s_Dist.reset();
    }

    void Log::AddSink(const spdlog::sink_ptr& sink) {
        if (s_Dist) s_Dist->add_sink(sink);
    }

}
