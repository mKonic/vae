#pragma once

#include <memory>

// spdlog is compiled (SPDLOG_COMPILED_LIB, see vendor-build/spdlog.lua) rather than header-only,
// because the header-only form costs ~1.9s of parse per translation unit that pulls it in.
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

namespace vae {

    // Two loggers: "VAE" for the engine, "APP" for whatever is built on top of it (Studio, Player,
    // a user's app). A sink can be attached later so Studio's Console panel shows both.
    class Log {
    public:
        static void Init();
        static void Shutdown();

        static std::shared_ptr<spdlog::logger>& CoreLogger() { return s_CoreLogger; }
        static std::shared_ptr<spdlog::logger>& AppLogger()  { return s_AppLogger; }

        // Studio's Console panel installs a sink here; harmless when nothing is listening.
        static void AddSink(const spdlog::sink_ptr& sink);

    private:
        static std::shared_ptr<spdlog::logger> s_CoreLogger;
        static std::shared_ptr<spdlog::logger> s_AppLogger;
    };

}

#define VAE_CORE_TRACE(...) ::vae::Log::CoreLogger()->trace(__VA_ARGS__)
#define VAE_CORE_INFO(...)  ::vae::Log::CoreLogger()->info(__VA_ARGS__)
#define VAE_CORE_WARN(...)  ::vae::Log::CoreLogger()->warn(__VA_ARGS__)
#define VAE_CORE_ERROR(...) ::vae::Log::CoreLogger()->error(__VA_ARGS__)
#define VAE_CORE_FATAL(...) ::vae::Log::CoreLogger()->critical(__VA_ARGS__)

#define VAE_TRACE(...) ::vae::Log::AppLogger()->trace(__VA_ARGS__)
#define VAE_INFO(...)  ::vae::Log::AppLogger()->info(__VA_ARGS__)
#define VAE_WARN(...)  ::vae::Log::AppLogger()->warn(__VA_ARGS__)
#define VAE_ERROR(...) ::vae::Log::AppLogger()->error(__VA_ARGS__)
#define VAE_FATAL(...) ::vae::Log::AppLogger()->critical(__VA_ARGS__)
