#include "Test.h"

#include "vae/svc/Services.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace vae;
using namespace vae::svc;

namespace {

    namespace fs = std::filesystem;

    fs::path Scratch(std::string_view name) {
        const fs::path dir = fs::temp_directory_path() / "vae-svc-tests" / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
    }

    // A real HTTP server on a real socket, started for the test and gone when it ends. The suite
    // must never reach the internet — a test that fails on a train is a test nobody trusts — but a
    // client tested against a mock is a client tested against the mock.
    class Fixture {
    public:
        Fixture() {
            m_Server.Get("/hello", [](const httplib::Request&, httplib::Response& response) {
                response.set_content("world", "text/plain");
            });
            m_Server.Get("/json", [](const httplib::Request&, httplib::Response& response) {
                response.set_content(R"({"answer":42})", "application/json");
            });
            m_Server.Get("/slow", [](const httplib::Request&, httplib::Response& response) {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                response.set_content("eventually", "text/plain");
            });
            m_Server.Get("/teapot", [](const httplib::Request&, httplib::Response& response) {
                response.status = 418;
                response.set_content("no coffee", "text/plain");
            });
            m_Server.Post("/echo", [](const httplib::Request& request, httplib::Response& response) {
                response.set_content(request.body, "text/plain");
            });
            m_Server.Get("/headers", [](const httplib::Request& request, httplib::Response& response) {
                response.set_content(request.get_header_value("X-Vae"), "text/plain");
            });

            m_Port = m_Server.bind_to_any_port("127.0.0.1");
            m_Thread = std::thread([this] { m_Server.listen_after_bind(); });
            m_Server.wait_until_ready();
        }

        ~Fixture() {
            m_Server.stop();
            if (m_Thread.joinable()) m_Thread.join();
        }

        std::string Url(std::string_view path) const {
            return "http://127.0.0.1:" + std::to_string(m_Port) + std::string(path);
        }
        bool Up() const { return m_Port > 0; }

    private:
        httplib::Server m_Server;
        std::thread m_Thread;
        int m_Port = 0;
    };

    // Pumps until the answers arrive or the patience runs out. Never a bare sleep: a fixed wait is
    // either too short on a loaded machine or wasted time on an idle one.
    bool PumpUntil(Http& http, const std::function<bool()>& done, int millis = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
        while (std::chrono::steady_clock::now() < deadline) {
            http.Pump();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        http.Pump();
        return done();
    }

}

// --------------------------------------------------------------------------------- storage

TEST(svc, storage_round_trips_every_value_a_script_can_hold) {
    const fs::path dir = Scratch("storage");
    {
        Storage store;
        store.Open(dir / "state.json");
        store.Set("name", std::string("Ada"));
        store.Set("score", 42.0f);
        store.Set("won", true);
        store.Set("where", Vec2{ 12.0f, 34.0f });
        store.Set("tint", Color{ 0.1f, 0.2f, 0.3f, 1.0f });
        CHECK(store.Dirty());
        CHECK(store.Flush());
        CHECK(!store.Dirty());
    }

    Storage reopened;
    reopened.Open(dir / "state.json");
    CHECK(reopened.Count() == 5);
    CHECK(std::get<std::string>(reopened.Get("name")) == "Ada");
    CHECK(std::get<f32>(reopened.Get("score")) == 42.0f);
    CHECK(std::get<bool>(reopened.Get("won")) == true);
    CHECK(std::get<Vec2>(reopened.Get("where")).y == 34.0f);
    CHECK(std::get<Color>(reopened.Get("tint")).b == 0.3f);
}

TEST(svc, a_storage_file_is_readable_by_a_person) {
    // The point of JSON over a binary blob: when something is wrong you open the file.
    const fs::path dir = Scratch("readable");
    Storage store;
    store.Open(dir / "state.json");
    store.Set("score", 7.0f);
    store.Flush();

    std::ifstream file(dir / "state.json");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"score\"") != std::string::npos);
    CHECK(text.find("7") != std::string::npos);
    CHECK(text.find("\"type\"") == std::string::npos);   // no wrapper objects
}

TEST(svc, writing_a_value_it_already_has_is_not_a_change) {
    const fs::path dir = Scratch("clean");
    Storage store;
    store.Open(dir / "state.json");
    store.Set("score", 1.0f);
    store.Flush();

    store.Set("score", 1.0f);
    CHECK(!store.Dirty());
    store.Set("score", 2.0f);
    CHECK(store.Dirty());
}

TEST(svc, a_missing_or_broken_storage_file_starts_empty_rather_than_failing) {
    const fs::path dir = Scratch("broken");
    { std::ofstream out(dir / "junk.json"); out << "{ this is not json"; }

    Storage store;
    store.Open(dir / "junk.json");
    CHECK(store.Count() == 0);
    store.Set("fresh", 1.0f);
    CHECK(store.Flush());

    Storage reopened;
    reopened.Open(dir / "junk.json");
    CHECK(reopened.Has("fresh"));
}

// ----------------------------------------------------------------------------------- files

TEST(svc, files_are_sandboxed_to_the_roots_the_app_was_given) {
    const fs::path dir = Scratch("files");
    fs::create_directories(dir / "assets");
    { std::ofstream out(dir / "assets" / "note.txt"); out << "inside"; }

    Files files;
    files.AddRoot(dir);

    const auto read = files.Read("assets/note.txt");
    CHECK(read.has_value() && *read == "inside");

    // Every way out of the sandbox anyone tries.
    CHECK(!files.Read("../../../etc/passwd").has_value());
    CHECK(!files.Read("/etc/passwd").has_value());
    CHECK(!files.Read("assets/../../outside.txt").has_value());
    CHECK(files.Resolve("../..").empty());
    CHECK(!files.Exists("/etc/hostname"));
}

TEST(svc, a_sibling_directory_with_a_shared_prefix_is_still_outside) {
    // "/data-other" starts with "/data" and is nowhere near inside it. A sandbox that compares
    // strings rather than paths lets exactly this through.
    const fs::path base = Scratch("prefix");
    fs::create_directories(base / "data");
    fs::create_directories(base / "data-other");
    { std::ofstream out(base / "data-other" / "secret.txt"); out << "no"; }

    Files files;
    files.AddRoot(base / "data");
    CHECK(!files.Read("../data-other/secret.txt").has_value());
}

TEST(svc, files_read_write_list_and_remove) {
    const fs::path dir = Scratch("rw");
    Files files;
    files.AddRoot(dir);

    CHECK(files.Write("notes/one.txt", "first"));
    CHECK(files.Write("notes/two.txt", "second"));
    CHECK(files.Exists("notes/one.txt"));

    const auto listing = files.List("notes");
    CHECK(listing.size() == 2);
    CHECK(listing[0] == "one.txt" && listing[1] == "two.txt");

    CHECK(files.Remove("notes/one.txt"));
    CHECK(!files.Exists("notes/one.txt"));
    CHECK(files.List("notes").size() == 1);
}

// ------------------------------------------------------------------------------------ http

TEST(svc, http_gets_a_body_from_a_real_server) {
    Fixture server;
    CHECK(server.Up());
    if (!server.Up()) return;

    Http http;
    const Response response = http.SendNow({ "GET", server.Url("/hello") });
    CHECK_MESSAGE(response.Ok(), response.error + " status=" + std::to_string(response.status));
    CHECK(response.body == "world");
}

TEST(svc, an_async_request_comes_back_on_the_thread_that_asked) {
    Fixture server;
    if (!server.Up()) return;

    Http http;
    const std::thread::id caller = std::this_thread::get_id();

    std::string body;
    std::thread::id delivered;
    bool arrived = false;
    http.Send({ "GET", server.Url("/json") }, [&](const Response& response) {
        body = response.body;
        delivered = std::this_thread::get_id();
        arrived = true;
    });

    CHECK(PumpUntil(http, [&] { return arrived; }));
    CHECK(body == R"({"answer":42})");
    // The whole reason for the pump: a script's callback runs where the script runs.
    CHECK(delivered == caller);
    CHECK(http.Pending() == 0);
}

TEST(svc, several_requests_in_flight_all_come_back) {
    Fixture server;
    if (!server.Up()) return;

    Http http;
    std::atomic<int> answered{ 0 };
    for (int i = 0; i < 8; ++i)
        http.Send({ "GET", server.Url("/hello") }, [&](const Response& r) {
            if (r.Ok() && r.body == "world") ++answered;
        });

    CHECK(PumpUntil(http, [&] { return answered.load() == 8; }));
    CHECK_MESSAGE(answered.load() == 8, std::to_string(answered.load()) + " of 8");
}

TEST(svc, a_post_carries_its_body_and_headers) {
    Fixture server;
    if (!server.Up()) return;

    Http http;
    Request request{ "POST", server.Url("/echo") };
    request.body = "the payload";
    request.contentType = "text/plain";
    const Response echo = http.SendNow(request);
    CHECK(echo.Ok());
    CHECK(echo.body == "the payload");

    Request tagged{ "GET", server.Url("/headers") };
    tagged.headers["X-Vae"] = "present";
    const Response seen = http.SendNow(tagged);
    CHECK(seen.body == "present");
}

TEST(svc, a_failure_is_reported_rather_than_thrown) {
    Http http;

    // Nothing listening. The port is one nobody binds by convention.
    const Response refused = http.SendNow({ "GET", "http://127.0.0.1:9/nothing" });
    CHECK(!refused.Ok());
    CHECK(!refused.error.empty());

    const Response nonsense = http.SendNow({ "GET", "not a url at all" });
    CHECK(!nonsense.Ok());
    CHECK(nonsense.error.find("url") != std::string::npos);
}

TEST(svc, a_status_that_is_not_success_is_still_an_answer) {
    Fixture server;
    if (!server.Up()) return;

    Http http;
    const Response response = http.SendNow({ "GET", server.Url("/teapot") });
    CHECK(!response.Ok());
    CHECK(response.status == 418);
    CHECK(response.body == "no coffee");
    CHECK(response.error.empty());        // it answered; it just said no
}

TEST(svc, the_network_can_be_turned_off_entirely) {
    Fixture server;
    if (!server.Up()) return;

    Http http;
    http.SetEnabled(false);
    const Response response = http.SendNow({ "GET", server.Url("/hello") });
    CHECK(!response.Ok());
    CHECK(!response.error.empty());
}

TEST(svc, a_cancelled_request_does_not_call_back) {
    Fixture server;
    if (!server.Up()) return;

    Http http;
    bool called = false;
    const u64 id = http.Send({ "GET", server.Url("/slow") }, [&](const Response&) { called = true; });
    http.Cancel(id);

    CHECK(PumpUntil(http, [&] { return http.Pending() == 0; }));
    CHECK(!called);
}

// -------------------------------------------------------------------------------- services

TEST(svc, the_clock_advances_with_the_frames) {
    Services services;
    CHECK(services.Uptime() == 0.0);
    for (int i = 0; i < 60; ++i) services.Tick(1.0f / 60.0f);
    CHECK(services.Uptime() > 0.99 && services.Uptime() < 1.01);

    CHECK(services.Now() > 1.7e9);        // some time after 2023, which is a low bar
    CHECK(services.Date("%Y").size() == 4);
}

TEST(svc, a_tick_flushes_storage_and_delivers_answers) {
    Fixture server;
    if (!server.Up()) return;

    const fs::path dir = Scratch("services");
    Services services;
    services.Store().Open(dir / "state.json");
    services.FileSystem().AddRoot(dir);

    services.Store().Set("visits", 1.0f);
    bool arrived = false;
    services.Net().Send({ "GET", server.Url("/hello") }, [&](const Response&) { arrived = true; });

    for (int i = 0; i < 2000 && !arrived; ++i) {
        services.Tick(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(arrived);
    CHECK(!services.Store().Dirty());     // flushed by the tick, not by the caller remembering to
    CHECK(fs::exists(dir / "state.json"));
}
