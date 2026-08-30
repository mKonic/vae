#pragma once

#include "vae/doc/Value.h"
#include "vae/svc/Audio.h"
#include "vae/svc/Socket.h"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vae::svc {

    // What an app can reach outside itself: files, saved state, the network, the clock.
    //
    // One surface, bound identically to both scripting languages, because "which services exist"
    // must not depend on which language a project happened to pick. Everything here is either
    // immediate or delivered back on the main thread — a script never sees a worker thread, and
    // never has to think about one.
    class Services;

    // --- storage ---------------------------------------------------------------------------------
    // Small, durable, key-value. Written as JSON next to the project so it survives a restart and
    // can be read by a human when something is wrong.
    class Storage {
    public:
        void Open(std::filesystem::path path);
        const std::filesystem::path& Path() const { return m_Path; }

        doc::Value Get(const std::string& key) const;
        void Set(const std::string& key, doc::Value value);
        bool Has(const std::string& key) const;
        void Remove(const std::string& key);
        void Clear();
        std::vector<std::string> Keys() const;
        std::size_t Count() const { return m_Values.size(); }

        // Written on change, but coalesced: a script that saves on every keystroke must not turn
        // into a script that writes a file on every keystroke.
        bool Flush();
        bool Dirty() const { return m_Dirty; }

    private:
        bool Load();

        std::filesystem::path m_Path;
        std::map<std::string, doc::Value> m_Values;
        bool m_Dirty = false;
    };

    // --- files -----------------------------------------------------------------------------------
    // Sandboxed by construction: every path is resolved inside the roots the app was given, and one
    // that escapes them is refused. An app builder that lets a script read /etc/shadow because
    // someone typed "../.." is not a thing to ship.
    class Files {
    public:
        void AddRoot(std::filesystem::path root);
        const std::vector<std::filesystem::path>& Roots() const { return m_Roots; }

        // Empty when the path is outside every root, which is also the answer for "does not exist" —
        // a sandbox that distinguishes the two leaks the shape of the filesystem.
        std::filesystem::path Resolve(std::string_view relative) const;

        std::optional<std::string> Read(std::string_view path) const;
        bool Write(std::string_view path, std::string_view text);
        bool Exists(std::string_view path) const;
        bool Remove(std::string_view path);
        std::vector<std::string> List(std::string_view directory) const;

    private:
        std::vector<std::filesystem::path> m_Roots;
    };

    // --- http ------------------------------------------------------------------------------------
    struct Response {
        int status = 0;                 // 0 means the request never got an answer
        std::string body;
        std::string error;
        std::map<std::string, std::string> headers;
        bool Ok() const { return status >= 200 && status < 300; }
    };

    struct Request {
        std::string method = "GET";
        std::string url;
        std::string body;
        std::string contentType = "application/json";
        std::map<std::string, std::string> headers;
        f32 timeout = 10.0f;
    };

    // Requests run on worker threads and answers are handed back on the main thread when Pump is
    // called. That is the whole contract: a script's callback runs where the script runs, so it can
    // touch the document without a lock and without a class of bug nobody can reproduce.
    // Whether this build can speak https. Decided at compile time by whether OpenSSL was found,
    // reported at runtime because everything downstream — an error message, an exported project's
    // link line — has to agree with what the engine was actually built with.
    bool HasTls();

    class Http {
    public:
        using Handler = std::function<void(const Response&)>;

        Http();
        ~Http();
        Http(const Http&) = delete;
        Http& operator=(const Http&) = delete;

        u64 Send(Request request, Handler handler);
        // Blocking, for tests and for tools. Never call it from a frame.
        Response SendNow(const Request& request);

        // Delivers whatever has come back. Returns how many answers were handed over.
        std::size_t Pump();
        std::size_t Pending() const;
        void Cancel(u64 id);
        void CancelAll();

        // Off by default in tests: a suite that reaches the network is a suite that fails on a train.
        void SetEnabled(bool enabled) { m_Enabled = enabled; }
        bool Enabled() const { return m_Enabled; }

    private:
        struct Answer {
            u64 id = 0;
            Response response;
            Handler handler;
        };

        mutable std::mutex m_Mutex;
        std::vector<Answer> m_Answers;
        std::vector<u64> m_Cancelled;
        std::size_t m_InFlight = 0;
        u64 m_NextId = 1;
        bool m_Enabled = true;
    };

    // --- everything, in one place ----------------------------------------------------------------
    class Services {
    public:
        Storage& Store() { return m_Storage; }
        Files& FileSystem() { return m_Files; }
        Http& Net() { return m_Http; }
        // Sound. Opens no device until something is played, so this being here costs a project
        // with no audio in it nothing at all.
        Audio& Sound() { return m_Audio; }

        // Live connections, by the name the app gave them. Named rather than numbered because a
        // script says "the prices feed", not "socket 2", and because a reload has to find the same
        // one again.
        Socket& Live(const std::string& name);
        Socket* FindLive(const std::string& name);
        void CloseLive(const std::string& name);
        void CloseAllLive();
        const std::map<std::string, Scope<Socket>>& LiveSockets() const { return m_Sockets; }

        // Seconds since the app started, and wall-clock seconds since the epoch. Both are what a
        // script means by "time", and which one it means depends entirely on what it is doing.
        f64 Uptime() const { return m_Uptime; }
        f64 Now() const;
        // A date, formatted with strftime's vocabulary because everyone already knows it.
        std::string Date(std::string_view format = "%Y-%m-%d %H:%M:%S") const;

        // Once a frame: advances the clock and delivers whatever the network brought back.
        void Tick(f32 dt);

        // Whether something is on its way in. An app waiting on an answer has to keep drawing
        // frames or the answer arrives into a window that has stopped looking.
        bool Busy() const { return m_Http.Pending() > 0 || !m_Sockets.empty(); }

    private:
        Storage m_Storage;
        Files m_Files;
        Http m_Http;
        Audio m_Audio;
        std::map<std::string, Scope<Socket>> m_Sockets;
        f64 m_Uptime = 0.0;
    };

}
