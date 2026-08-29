#include "vaepch.h"
#include "vae/base/Log.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Platform.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>
#include <spdlog/sinks/dist_sink.h>

namespace vae {

    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_AppLogger;
    std::shared_ptr<spdlog::sinks::sink> Log::s_File;
    std::filesystem::path Log::s_Path;

    namespace { std::shared_ptr<spdlog::sinks::dist_sink_mt> s_Dist; }

    namespace {

        // How many finished runs are kept beside the current one. A file per process trades the
        // old fixed two files for an unbounded pile, so something has to sweep up; five is enough
        // to still have the run before the one that went wrong.
        constexpr std::size_t kKeepRuns = 5;

        bool AllDigits(std::string_view text) {
            if (text.empty()) return false;
            return std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
        }

        // "vae-studio-4131.log" and "vae-studio-4131.2.log" are the same run: the rotation index
        // spdlog inserts before the extension is not part of the name. Returns the shared part and
        // the pid in it, or nothing at all for a file this code did not write.
        struct Named { std::string run; std::uint32_t pid = 0; };

        std::optional<Named> ParseLogName(const std::filesystem::path& file) {
            if (file.extension() != ".log") return std::nullopt;
            std::string base = file.stem().string();

            // Strip the rotation index, which is a trailing ".<digits>". A program whose own name
            // ends in a number is why the pid is joined with a dash and this with a dot.
            if (const auto dot = base.rfind('.'); dot != std::string::npos
                                               && AllDigits(std::string_view(base).substr(dot + 1)))
                base.resize(dot);

            // The one fixed name every process used to share, left behind by an install from
            // before this. Pid 0 is never alive, so it ages out with everything else rather than
            // sitting in the directory forever.
            if (base == "vae") return Named{ base, 0 };

            const auto dash = base.rfind('-');
            if (dash == std::string::npos) return std::nullopt;
            const std::string_view digits = std::string_view(base).substr(dash + 1);
            if (!AllDigits(digits)) return std::nullopt;

            Named named{ base, 0 };
            std::from_chars(digits.data(), digits.data() + digits.size(), named.pid);
            return named;
        }

    }

    std::string Log::FileNameFor(std::string_view program, std::uint32_t pid) {
        std::string name;
        name.reserve(program.size() + 8);
        for (const char c : program) {
            const unsigned char u = static_cast<unsigned char>(c);
            // A file name, not a program name: an app is called whatever its author called it,
            // and "My App (2).log" is a worse thing to type than "my-app-2-4131.log".
            if (std::isalnum(u)) name += static_cast<char>(std::tolower(u));
            else if (!name.empty() && name.back() != '-') name += '-';
        }
        while (!name.empty() && name.back() == '-') name.pop_back();
        if (name.empty()) name = "vae";
        return name + "-" + std::to_string(pid) + ".log";
    }

    void Log::Prune(const std::filesystem::path& dir, std::size_t keep,
                    const std::function<bool(std::uint32_t)>& alive) {
        struct Run {
            std::vector<std::filesystem::path>      files;
            // `min()`, not a default-constructed value: file_clock's epoch is in the future on
            // libstdc++, so every real timestamp is negative and a zero seed wins every max().
            std::filesystem::file_time_type         newest = std::filesystem::file_time_type::min();
            bool                                    live = false;
        };
        std::map<std::string, Run> runs;

        std::error_code ec;
        std::filesystem::directory_iterator entries(dir, ec);
        if (ec) return;                 // no directory yet, on the first run of a fresh install

        for (const auto& entry : entries) {
            std::error_code each;
            if (!entry.is_regular_file(each) || each) continue;
            const auto named = ParseLogName(entry.path());
            if (!named) continue;

            Run& run = runs[named->run];
            if (run.files.empty()) run.live = alive(named->pid);
            run.files.push_back(entry.path());
            const auto written = entry.last_write_time(each);
            if (!each) run.newest = std::max(run.newest, written);
        }

        // Only finished runs are candidates: deleting the file a live process holds open unlinks
        // it without stopping the writing, so the run would go on logging into nothing.
        std::vector<const Run*> finished;
        for (const auto& [name, run] : runs)
            if (!run.live) finished.push_back(&run);

        std::ranges::sort(finished, [](const Run* a, const Run* b) { return a->newest > b->newest; });
        for (std::size_t i = keep; i < finished.size(); ++i)
            for (const auto& file : finished[i]->files)
                std::filesystem::remove(file, ec);
    }

    void Log::Init() {
        if (s_CoreLogger) return;

        // A dist_sink lets Studio attach its Console sink after the loggers already exist,
        // instead of every logger having to be rebuilt when a panel opens.
        s_Dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        s_Dist->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        // And to a file, because a console is whatever was attached when it happened. A crash on
        // somebody else's machine leaves the last run behind either way: two files, rotated, so
        // the previous session survives the one that replaced it.
        //
        // One file *per process*, named for the program and its pid. The Studio and the app it
        // launches (Ctrl+F5) are two writers, and `_mt` only makes a sink safe across the threads
        // of one process — two processes rotating the same file is one of them renaming the file
        // the other is holding open, and the losing side then writes into a file nobody can find.
        try {
            const std::filesystem::path dir = FileSystem::ConfigRoot() / "logs";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            // Before opening ours, not after: sweeping up the runs that ended is what keeps a file
            // per process from being an unbounded pile, and our own file does not exist yet.
            Prune(dir, kKeepRuns, platform::ProcessIdAlive);

            const std::string program = FileSystem::ExecutablePath().stem().string();
            s_Path = dir / FileNameFor(program, platform::CurrentProcessId());
            s_File = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                s_Path.string(), 2 * 1024 * 1024, 2, true);
            s_Dist->add_sink(s_File);
        } catch (const std::exception& e) {
            s_Path.clear();
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
        s_Path.clear();
        s_File.reset();
        s_CoreLogger.reset();
        s_AppLogger.reset();
        s_Dist.reset();
    }

    void Log::AddSink(const spdlog::sink_ptr& sink) {
        if (s_Dist) s_Dist->add_sink(sink);
    }

}
