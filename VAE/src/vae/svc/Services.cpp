#include "vaepch.h"
#include "vae/svc/Services.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>

// CPPHTTPLIB_OPENSSL_SUPPORT is set by the build, not here. httplib is header-only, so every
// translation unit that includes it has to agree about the configuration or the layouts diverge.
#include <httplib.h>

namespace vae::svc {

    bool HasTls() {
#ifdef VAE_HTTP_TLS
        return true;
#else
        return false;
#endif
    }


    namespace {

        using json = nlohmann::json;

        // Plain JSON, not the document format's tagged encoding. A storage file is meant to be
        // opened and read when something is wrong, and `{"score": 42}` is that; a wrapper object
        // per value is not.
        json Encode(const doc::Value& value) {
            switch (doc::TypeOf(value)) {
                case doc::ValueType::Bool:   return std::get<bool>(value);
                case doc::ValueType::Number: return std::get<f32>(value);
                case doc::ValueType::Text:   return std::get<std::string>(value);
                case doc::ValueType::Vector2: {
                    const Vec2 v = std::get<Vec2>(value);
                    return json::array({ v.x, v.y });
                }
                case doc::ValueType::Colour: {
                    const Color c = std::get<Color>(value);
                    return json::array({ c.r, c.g, c.b, c.a });
                }
                default: return nullptr;
            }
        }

        doc::Value Decode(const json& node) {
            if (node.is_boolean())       return node.get<bool>();
            if (node.is_number())        return node.get<f32>();
            if (node.is_string())        return node.get<std::string>();
            if (node.is_array() && node.size() == 2)
                return Vec2{ node[0].get<f32>(), node[1].get<f32>() };
            if (node.is_array() && node.size() == 4)
                return Color{ node[0].get<f32>(), node[1].get<f32>(),
                              node[2].get<f32>(), node[3].get<f32>() };
            return {};
        }

    }

    // ------------------------------------------------------------------------------- storage

    void Storage::Open(std::filesystem::path path) {
        Flush();
        m_Path = std::move(path);
        m_Values.clear();
        m_Dirty = false;
        Load();
    }

    bool Storage::Load() {
        const auto text = vae::FileSystem::ReadText(m_Path);
        if (!text) return false;

        json parsed = json::parse(*text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            VAE_CORE_WARN("storage: {} is not readable — starting empty", m_Path.string());
            return false;
        }
        for (const auto& [key, value] : parsed.items()) m_Values[key] = Decode(value);
        return true;
    }

    doc::Value Storage::Get(const std::string& key) const {
        const auto it = m_Values.find(key);
        return it == m_Values.end() ? doc::Value{} : it->second;
    }

    void Storage::Set(const std::string& key, doc::Value value) {
        if (const auto it = m_Values.find(key); it != m_Values.end() && it->second == value) return;
        m_Values[key] = std::move(value);
        m_Dirty = true;
    }

    bool Storage::Has(const std::string& key) const { return m_Values.contains(key); }

    void Storage::Remove(const std::string& key) {
        if (m_Values.erase(key) > 0) m_Dirty = true;
    }

    void Storage::Clear() {
        if (m_Values.empty()) return;
        m_Values.clear();
        m_Dirty = true;
    }

    std::vector<std::string> Storage::Keys() const {
        std::vector<std::string> keys;
        keys.reserve(m_Values.size());
        for (const auto& [key, value] : m_Values) keys.push_back(key);
        return keys;
    }

    bool Storage::Flush() {
        if (!m_Dirty || m_Path.empty()) return false;

        json out = json::object();
        for (const auto& [key, value] : m_Values) out[key] = Encode(value);

        std::error_code ec;
        std::filesystem::create_directories(m_Path.parent_path(), ec);
        if (!vae::FileSystem::WriteText(m_Path, out.dump(2))) {
            VAE_CORE_ERROR("storage: could not write {}", m_Path.string());
            return false;
        }
        m_Dirty = false;
        return true;
    }

    // --------------------------------------------------------------------------------- files

    void Files::AddRoot(std::filesystem::path root) {
        std::error_code ec;
        std::filesystem::path resolved = std::filesystem::weakly_canonical(root, ec);
        if (ec) resolved = std::move(root);
        if (std::ranges::find(m_Roots, resolved) == m_Roots.end())
            m_Roots.push_back(std::move(resolved));
    }

    std::filesystem::path Files::Resolve(std::string_view relative) const {
        if (relative.empty()) return {};

        std::error_code ec;
        for (const std::filesystem::path& root : m_Roots) {
            const std::filesystem::path candidate =
                std::filesystem::weakly_canonical(root / relative, ec);
            if (ec) { ec.clear(); continue; }

            // Compared component-wise, not as strings: "/data-other" starts with "/data" and is
            // nowhere near inside it.
            const auto rootParts = std::distance(root.begin(), root.end());
            if (std::distance(candidate.begin(), candidate.end()) < rootParts) continue;
            if (std::equal(root.begin(), root.end(), candidate.begin())) return candidate;
        }
        return {};
    }

    std::optional<std::string> Files::Read(std::string_view path) const {
        const std::filesystem::path resolved = Resolve(path);
        if (resolved.empty()) return std::nullopt;
        return vae::FileSystem::ReadText(resolved);
    }

    bool Files::Write(std::string_view path, std::string_view text) {
        const std::filesystem::path resolved = Resolve(path);
        if (resolved.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(resolved.parent_path(), ec);
        return vae::FileSystem::WriteText(resolved, text);
    }

    bool Files::Exists(std::string_view path) const {
        const std::filesystem::path resolved = Resolve(path);
        if (resolved.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(resolved, ec);
    }

    bool Files::Remove(std::string_view path) {
        const std::filesystem::path resolved = Resolve(path);
        if (resolved.empty()) return false;
        std::error_code ec;
        return std::filesystem::remove(resolved, ec);
    }

    std::vector<std::string> Files::List(std::string_view directory) const {
        std::vector<std::string> names;
        const std::filesystem::path resolved = Resolve(directory.empty() ? "." : directory);
        if (resolved.empty()) return names;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(resolved, ec))
            names.push_back(entry.path().filename().string());
        std::ranges::sort(names);
        return names;
    }

    // ---------------------------------------------------------------------------------- http

    Http::Http() = default;

    Http::~Http() {
        // Nothing to join: every request owns a detached future that writes into a shared, locked
        // queue and the queue outlives them by construction... which it does not, so wait instead.
        CancelAll();
    }

    namespace {

        // scheme://host[:port] and the path, because httplib wants them separately.
        bool SplitUrl(const std::string& url, std::string& base, std::string& path) {
            const std::size_t scheme = url.find("://");
            if (scheme == std::string::npos) return false;
            const std::size_t slash = url.find('/', scheme + 3);
            if (slash == std::string::npos) {
                base = url;
                path = "/";
            } else {
                base = url.substr(0, slash);
                path = url.substr(slash);
            }
            return true;
        }

    }

    Response Http::SendNow(const Request& request) {
        Response response;
        if (!m_Enabled) {
            response.error = "the network is turned off for this app";
            return response;
        }

        std::string base, path;
        if (!SplitUrl(request.url, base, path)) {
            response.error = "not a url: " + request.url;
            return response;
        }

#ifndef VAE_HTTP_TLS
        if (base.starts_with("https://")) {
            response.error = "this build has no TLS — rebuild with OpenSSL for https";
            return response;
        }
#endif

        try {
            httplib::Client client(base);
            client.set_follow_location(true);
            client.set_connection_timeout(static_cast<time_t>(request.timeout), 0);
            client.set_read_timeout(static_cast<time_t>(request.timeout), 0);

            httplib::Headers headers;
            for (const auto& [key, value] : request.headers) headers.emplace(key, value);

            httplib::Result result =
                request.method == "POST"   ? client.Post(path, headers, request.body, request.contentType)
              : request.method == "PUT"    ? client.Put(path, headers, request.body, request.contentType)
              : request.method == "DELETE" ? client.Delete(path, headers, request.body, request.contentType)
                                           : client.Get(path, headers);

            if (!result) {
                response.error = httplib::to_string(result.error());
                return response;
            }
            response.status = result->status;
            response.body = result->body;
            for (const auto& [key, value] : result->headers) response.headers[key] = value;
        } catch (const std::exception& e) {
            response.error = e.what();
        }
        return response;
    }

    u64 Http::Send(Request request, Handler handler) {
        u64 id;
        {
            std::lock_guard lock(m_Mutex);
            id = m_NextId++;
            ++m_InFlight;
        }

        // Detached, and everything it touches is either owned by the task or guarded. The answer
        // waits in the queue until the main thread comes to collect it.
        std::thread([this, id, request = std::move(request), handler = std::move(handler)]() mutable {
            Response response = SendNow(request);
            std::lock_guard lock(m_Mutex);
            --m_InFlight;
            if (std::ranges::find(m_Cancelled, id) != m_Cancelled.end()) {
                std::erase(m_Cancelled, id);
                return;
            }
            m_Answers.push_back({ id, std::move(response), std::move(handler) });
        }).detach();

        return id;
    }

    std::size_t Http::Pump() {
        std::vector<Answer> ready;
        {
            std::lock_guard lock(m_Mutex);
            ready.swap(m_Answers);
        }
        // Handlers run outside the lock: one of them will call Send again, and a handler that
        // deadlocks the thing that called it is a trap nobody finds twice.
        for (Answer& answer : ready)
            if (answer.handler) answer.handler(answer.response);
        return ready.size();
    }

    std::size_t Http::Pending() const {
        std::lock_guard lock(m_Mutex);
        return m_InFlight + m_Answers.size();
    }

    void Http::Cancel(u64 id) {
        std::lock_guard lock(m_Mutex);
        std::erase_if(m_Answers, [&](const Answer& answer) { return answer.id == id; });
        m_Cancelled.push_back(id);
    }

    void Http::CancelAll() {
        // Waits for what is in flight rather than abandoning it: a detached thread writing into a
        // queue that has been destroyed is the worst kind of crash to debug.
        for (int i = 0; i < 200; ++i) {
            {
                std::lock_guard lock(m_Mutex);
                m_Answers.clear();
                if (m_InFlight == 0) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        VAE_CORE_WARN("http: gave up waiting for {} request(s)", Pending());
    }

    // ------------------------------------------------------------------------------ services

    f64 Services::Now() const {
        using namespace std::chrono;
        return duration<f64>(system_clock::now().time_since_epoch()).count();
    }

    std::string Services::Date(std::string_view format) const {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);

        std::string pattern(format);
        char buffer[256];
        const std::size_t written = std::strftime(buffer, sizeof buffer, pattern.c_str(), &local);
        return std::string(buffer, written);
    }

    Socket& Services::Live(const std::string& name) {
        auto it = m_Sockets.find(name);
        if (it == m_Sockets.end()) it = m_Sockets.emplace(name, CreateScope<Socket>()).first;
        return *it->second;
    }

    Socket* Services::FindLive(const std::string& name) {
        const auto it = m_Sockets.find(name);
        return it == m_Sockets.end() ? nullptr : it->second.get();
    }

    void Services::CloseLive(const std::string& name) {
        if (const auto it = m_Sockets.find(name); it != m_Sockets.end()) {
            it->second->Close();
            m_Sockets.erase(it);
        }
    }

    void Services::CloseAllLive() { m_Sockets.clear(); }

    void Services::Tick(f32 dt) {
        m_Uptime += dt;
        m_Http.Pump();
        m_Audio.Pump();
        m_Storage.Flush();
    }

}
