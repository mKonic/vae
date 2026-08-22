#include "vaepch.h"
#include "vae/base/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/dist_sink.h>

namespace vae {

    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_AppLogger;

    namespace { std::shared_ptr<spdlog::sinks::dist_sink_mt> s_Dist; }

    void Log::Init() {
        if (s_CoreLogger) return;

        // A dist_sink lets Studio attach its Console sink after the loggers already exist,
        // instead of every logger having to be rebuilt when a panel opens.
        s_Dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        s_Dist->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        s_CoreLogger = std::make_shared<spdlog::logger>("VAE", s_Dist);
        s_AppLogger  = std::make_shared<spdlog::logger>("APP", s_Dist);

        for (auto* l : { &s_CoreLogger, &s_AppLogger }) {
            (*l)->set_pattern("%^[%T] %n: %v%$");
            (*l)->set_level(spdlog::level::trace);
            (*l)->flush_on(spdlog::level::trace);
            spdlog::register_logger(*l);
        }
    }

    void Log::Shutdown() {
        spdlog::drop_all();
        s_CoreLogger.reset();
        s_AppLogger.reset();
        s_Dist.reset();
    }

    void Log::AddSink(const spdlog::sink_ptr& sink) {
        if (s_Dist) s_Dist->add_sink(sink);
    }

}
