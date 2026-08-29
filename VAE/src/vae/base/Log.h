#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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

        // Where this process is writing, or empty when it has no file sink. Worth asking for:
        // there is no longer one path to tell somebody, because there is a file per process.
        static const std::filesystem::path& FilePath() { return s_Path; }

        // What this process's log file is called: "<program>-<pid>.log". The Studio and the app it
        // launches are two writers, and one rotating sink shared between two processes rotates
        // from underneath itself — a file per process is the whole of the fix.
        static std::string FileNameFor(std::string_view program, std::uint32_t pid);

        // Deletes the log files of runs that have finished, newest `keep` kept, and never touches
        // one whose process is still there. Separated out and public because this is the half of
        // the naming that can be tested: a directory and a liveness predicate in, what survives
        // out. `alive` is a parameter for that reason — the real one is platform::ProcessIdAlive.
        static void Prune(const std::filesystem::path& dir, std::size_t keep,
                          const std::function<bool(std::uint32_t)>& alive);

    private:
        static std::filesystem::path s_Path;
        static std::shared_ptr<spdlog::logger> s_CoreLogger;
        static std::shared_ptr<spdlog::logger> s_AppLogger;
        // The rotating file sink, kept so its pattern can be set after the loggers set theirs.
        static std::shared_ptr<spdlog::sinks::sink> s_File;
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
