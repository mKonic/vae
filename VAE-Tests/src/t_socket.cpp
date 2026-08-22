#include "Test.h"

#include "vae/svc/Socket.h"
#include "vae/svc/Sockets.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace vae;

namespace {

    // A WebSocket server, only as much of one as a test needs: it accepts once, answers the
    // handshake, says something first, and echoes what it is told back in capitals. A local server
    // rather than a mocked socket, because the handshake and the framing are the parts most likely
    // to be wrong and a mock would agree with whatever the client did.
    class TinyServer {
    public:
        bool Start() {
            svc::EnsureStarted();
            m_Listen = ::socket(AF_INET, SOCK_STREAM, 0);
            if (!svc::Valid(m_Listen)) return false;
            int yes = 1;
            ::setsockopt(m_Listen, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&yes), sizeof yes);

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;                       // an ephemeral port: no fixed-port clashes
            if (::bind(m_Listen, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
                return false;
            socklen_t size = sizeof address;
            if (::getsockname(m_Listen, reinterpret_cast<sockaddr*>(&address), &size) != 0)
                return false;
            m_Port = ntohs(address.sin_port);
            if (::listen(m_Listen, 1) != 0) return false;

            m_Worker = std::thread(&TinyServer::Run, this);
            return true;
        }

        ~TinyServer() {
            m_Stop = true;
            if (svc::Valid(m_Listen)) ::shutdown(m_Listen, svc::kShutdownBoth);
            if (m_Worker.joinable()) m_Worker.join();
            if (svc::Valid(m_Listen)) svc::CloseFd(m_Listen);
        }

        std::string Url() const { return "ws://127.0.0.1:" + std::to_string(m_Port) + "/feed"; }
        // What the client sent, unmasked by the server — the other half of the framing contract.
        std::string Heard() const { return m_Heard; }
        bool Handshook() const { return m_Handshook.load(); }

    private:
        static std::string Frame(unsigned char opcode, std::string_view payload) {
            // A server never masks, which is the case a client that only handles masked frames
            // gets wrong.
            std::string out;
            out += static_cast<char>(0x80 | opcode);
            if (payload.size() < 126) {
                out += static_cast<char>(payload.size());
            } else {
                out += static_cast<char>(126);
                out += static_cast<char>((payload.size() >> 8) & 0xFF);
                out += static_cast<char>(payload.size() & 0xFF);
            }
            out.append(payload);
            return out;
        }

        void Run() {
            const svc::Fd client = ::accept(m_Listen, nullptr, nullptr);
            if (!svc::Valid(client)) return;

            std::string inbox;
            char chunk[2048];
            while (inbox.find("\r\n\r\n") == std::string::npos && !m_Stop) {
                const auto got = ::recv(client, chunk, sizeof chunk, 0);
                if (got <= 0) { svc::CloseFd(client); return; }
                inbox.append(chunk, static_cast<std::size_t>(got));
            }

            const std::size_t at = inbox.find("Sec-WebSocket-Key:");
            if (at == std::string::npos) { svc::CloseFd(client); return; }
            const std::size_t start = inbox.find_first_not_of(" ", at + 18);
            const std::size_t end = inbox.find("\r\n", start);
            const std::string key = inbox.substr(start, end - start);

            std::string reply = "HTTP/1.1 101 Switching Protocols\r\n";
            reply += "Upgrade: websocket\r\n";
            reply += "Connection: Upgrade\r\n";
            reply += "Sec-WebSocket-Accept: " + svc::AcceptToken(key) + "\r\n\r\n";
            ::send(client, reply.data(), static_cast<int>(reply.size()), svc::kNoSignal);
            m_Handshook = true;

            const std::string hello = Frame(0x1, "the server spoke first");
            ::send(client, hello.data(), static_cast<int>(hello.size()), svc::kNoSignal);

            // One frame back from the client, unmasked and echoed in capitals.
            inbox.clear();
            while (!m_Stop) {
                pollfd waiting{ client, POLLIN, 0 };
                if (svc::Poll(&waiting, 1, 50) <= 0) continue;
                const auto got = ::recv(client, chunk, sizeof chunk, 0);
                if (got <= 0) break;
                inbox.append(chunk, static_cast<std::size_t>(got));
                if (inbox.size() < 2) continue;

                const auto* bytes = reinterpret_cast<const unsigned char*>(inbox.data());
                const unsigned char opcode = bytes[0] & 0x0F;
                const bool masked = (bytes[1] & 0x80) != 0;
                std::size_t size = bytes[1] & 0x7F;
                std::size_t cursor = 2;
                if (size == 126) {
                    if (inbox.size() < 4) continue;
                    size = static_cast<std::size_t>(bytes[2]) << 8 | bytes[3];
                    cursor = 4;
                }
                unsigned char mask[4]{};
                if (masked) {
                    if (inbox.size() < cursor + 4) continue;
                    std::memcpy(mask, bytes + cursor, 4);
                    cursor += 4;
                }
                if (inbox.size() < cursor + size) continue;

                std::string payload = inbox.substr(cursor, size);
                if (masked)
                    for (std::size_t i = 0; i < payload.size(); ++i)
                        payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
                inbox.erase(0, cursor + size);

                if (opcode == 0x8) break;                       // the client said goodbye
                if (opcode != 0x1) continue;
                if (!masked) { m_Heard = "UNMASKED"; break; }   // a client that does not mask is broken

                m_Heard = payload;
                for (char& c : payload)
                    c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
                const std::string echo = Frame(0x1, payload);
                ::send(client, echo.data(), static_cast<int>(echo.size()), svc::kNoSignal);
            }
            svc::CloseFd(client);
        }

        svc::Fd m_Listen = svc::kNoFd;
        u16 m_Port = 0;
        std::thread m_Worker;
        std::atomic<bool> m_Stop{ false };
        std::atomic<bool> m_Handshook{ false };
        std::string m_Heard;
    };

    // Pumps until the condition holds or the patience runs out. Anything involving a socket needs
    // this: a fixed number of pumps is a race with the kernel.
    bool PumpUntil(svc::Socket& socket, std::vector<svc::Socket::Event>& into,
                   const std::function<bool()>& done, int millis = 4000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
        while (std::chrono::steady_clock::now() < deadline) {
            socket.Pump([&](const svc::Socket::Event& event) { into.push_back(event); });
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        socket.Pump([&](const svc::Socket::Event& event) { into.push_back(event); });
        return done();
    }

}

TEST(socket, the_accept_token_matches_the_one_the_protocol_specifies) {
    // RFC 6455's own worked example. A handshake that computes this wrong fails against every real
    // server and against nothing here, which is exactly the bug a local test server would hide.
    CHECK(svc::AcceptToken("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(socket, a_url_that_cannot_be_served_says_so_rather_than_hanging) {
    svc::Socket socket;
    std::string error;

    CHECK(!socket.Open("not a url", &error));
    CHECK(!error.empty());

    // Refused rather than quietly downgraded: a socket that says it is encrypted and is not is
    // worse than one that says it cannot be.
    CHECK(!socket.Open("wss://example.com/feed", &error));
    CHECK(error.find("wss") != std::string::npos);

    CHECK(!socket.Open("http://example.com/feed", &error));
    CHECK(!error.empty());
}

TEST(socket, a_server_that_speaks_first_reaches_the_main_thread) {
    TinyServer server;
    CHECK(server.Start());

    svc::Socket socket;
    std::string error;
    CHECK_MESSAGE(socket.Open(server.Url(), &error), error);

    std::vector<svc::Socket::Event> events;
    const auto sawKind = [&](svc::Socket::Event::Kind kind) {
        for (const auto& event : events) if (event.kind == kind) return true;
        return false;
    };

    CHECK(PumpUntil(socket, events, [&] {
        return sawKind(svc::Socket::Event::Kind::Message);
    }));
    CHECK(server.Handshook());
    CHECK(sawKind(svc::Socket::Event::Kind::Opened));

    std::string first;
    for (const auto& event : events)
        if (event.kind == svc::Socket::Event::Kind::Message) { first = event.text; break; }
    // The server said something without being asked, which is the entire reason for a socket.
    CHECK_MESSAGE(first == "the server spoke first", first);

    // And the client's own frames are masked, which is not optional: a server is required to hang
    // up on an unmasked one, and the failure looks like "it disconnects immediately".
    socket.Send("hello there");
    const std::size_t before = events.size();
    CHECK(PumpUntil(socket, events, [&] { return events.size() > before; }));
    CHECK_MESSAGE(events.back().text == "HELLO THERE", events.back().text);
    CHECK_MESSAGE(server.Heard() == "hello there", server.Heard());

    socket.Close();
    CHECK(socket.Status() == svc::Socket::State::Closed);
}

TEST(socket, a_server_that_is_not_there_fails_instead_of_waiting_forever) {
    svc::Socket socket;
    std::string error;
    // Port 1 on loopback: nothing listens there, and the refusal is immediate rather than a
    // timeout, which is the difference between a failure a script can show and a hung app.
    CHECK(socket.Open("ws://127.0.0.1:1/feed", &error));

    std::vector<svc::Socket::Event> events;
    CHECK(PumpUntil(socket, events, [&] { return !events.empty(); }));
    CHECK(events.front().kind == svc::Socket::Event::Kind::Failed);
    CHECK(!events.front().text.empty());
    CHECK(socket.Status() == svc::Socket::State::Failed);
}
