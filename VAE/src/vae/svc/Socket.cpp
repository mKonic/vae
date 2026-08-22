#include "vaepch.h"
#include "vae/svc/Socket.h"

#include "vae/svc/Sockets.h"

#include <array>
#include <cstring>
#include <random>

namespace vae::svc {

    namespace {

        // --- the two things the handshake needs ---------------------------------------------------
        // SHA-1 and base64, spelled out. Both are ~40 lines, both are only here to compute one
        // header, and vendoring a crypto library to check a handshake nobody attacks is not a trade.

        struct Sha1 {
            u32 h[5]{ 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
            std::vector<u8> buffer;

            static u32 Rotate(u32 value, int by) { return (value << by) | (value >> (32 - by)); }

            void Block(const u8* chunk) {
                u32 w[80];
                for (int i = 0; i < 16; ++i)
                    w[i] = static_cast<u32>(chunk[i * 4] << 24 | chunk[i * 4 + 1] << 16
                                          | chunk[i * 4 + 2] << 8 | chunk[i * 4 + 3]);
                for (int i = 16; i < 80; ++i)
                    w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

                u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; ++i) {
                    u32 f, k;
                    if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
                    else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1u; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDCu; }
                    else             { f = b ^ c ^ d;                      k = 0xCA62C1D6u; }
                    const u32 next = Rotate(a, 5) + f + e + k + w[i];
                    e = d; d = c; c = Rotate(b, 30); b = a; a = next;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
            }

            std::array<u8, 20> Digest(std::string_view input) {
                std::vector<u8> data(input.begin(), input.end());
                const u64 bits = static_cast<u64>(data.size()) * 8;
                data.push_back(0x80);
                while (data.size() % 64 != 56) data.push_back(0);
                for (int i = 7; i >= 0; --i) data.push_back(static_cast<u8>(bits >> (i * 8)));
                for (std::size_t at = 0; at < data.size(); at += 64) Block(data.data() + at);

                std::array<u8, 20> out{};
                for (int i = 0; i < 5; ++i)
                    for (int b = 0; b < 4; ++b)
                        out[static_cast<std::size_t>(i) * 4 + b] =
                            static_cast<u8>(h[i] >> (24 - b * 8));
                return out;
            }
        };

        std::string Base64(const u8* data, std::size_t size) {
            static constexpr char kAlphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((size + 2) / 3 * 4);
            for (std::size_t i = 0; i < size; i += 3) {
                const u32 a = data[i];
                const u32 b = i + 1 < size ? data[i + 1] : 0u;
                const u32 c = i + 2 < size ? data[i + 2] : 0u;
                const u32 triple = (a << 16) | (b << 8) | c;
                out += kAlphabet[(triple >> 18) & 0x3F];
                out += kAlphabet[(triple >> 12) & 0x3F];
                out += i + 1 < size ? kAlphabet[(triple >> 6) & 0x3F] : '=';
                out += i + 2 < size ? kAlphabet[triple & 0x3F] : '=';
            }
            return out;
        }

    }

    std::string AcceptToken(std::string_view key) {
        Sha1 sha;
        const auto digest = sha.Digest(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        return Base64(digest.data(), digest.size());
    }

    namespace {

        // --- url ----------------------------------------------------------------------------------

        bool SplitWs(const std::string& url, std::string& host, std::string& port,
                     std::string& path, std::string* error) {
            const std::size_t scheme = url.find("://");
            if (scheme == std::string::npos) {
                if (error) *error = "not a url: " + url;
                return false;
            }
            const std::string_view protocol(url.data(), scheme);
            if (protocol == "wss") {
                if (error) *error = "wss:// is not supported yet — this build speaks ws:// only";
                return false;
            }
            if (protocol != "ws") {
                if (error) *error = "not a websocket url: " + url;
                return false;
            }

            const std::size_t start = scheme + 3;
            const std::size_t slash = url.find('/', start);
            const std::string authority = url.substr(start, slash == std::string::npos
                                                          ? std::string::npos : slash - start);
            path = slash == std::string::npos ? "/" : url.substr(slash);
            if (authority.empty()) {
                if (error) *error = "no host in " + url;
                return false;
            }

            const std::size_t colon = authority.rfind(':');
            if (colon != std::string::npos && authority.find(']') == std::string::npos) {
                host = authority.substr(0, colon);
                port = authority.substr(colon + 1);
            } else {
                host = authority;
                port = "80";
            }
            return true;
        }

        Fd Connect(const std::string& host, const std::string& port) {
            EnsureStarted();

            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            addrinfo* found = nullptr;
            if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0) return -1;

            Fd fd = kNoFd;
            for (addrinfo* at = found; at; at = at->ai_next) {
                fd = ::socket(at->ai_family, at->ai_socktype, at->ai_protocol);
                if (!Valid(fd)) continue;
                if (::connect(fd, at->ai_addr, static_cast<int>(at->ai_addrlen)) == 0) break;
                CloseFd(fd);
                fd = kNoFd;
            }
            freeaddrinfo(found);
            return fd;
        }

        bool SendAll(Fd fd, const void* data, std::size_t size) {
            const auto* at = static_cast<const char*>(data);
            while (size > 0) {
                const auto wrote = ::send(fd, at, static_cast<int>(size), kNoSignal);
                if (wrote <= 0) return false;
                at += wrote;
                size -= static_cast<std::size_t>(wrote);
            }
            return true;
        }

        // --- framing --------------------------------------------------------------------------------

        constexpr u8 kText = 0x1, kBinary = 0x2, kClose = 0x8, kPing = 0x9, kPong = 0xA;

        // Every frame a client sends is masked; a server that gets an unmasked one is required to
        // hang up, which is a bug that looks like "the connection drops immediately".
        std::string Frame(u8 opcode, std::string_view payload, u32 mask) {
            std::string out;
            out += static_cast<char>(0x80 | opcode);

            const std::size_t size = payload.size();
            if (size < 126) {
                out += static_cast<char>(0x80 | size);
            } else if (size <= 0xFFFF) {
                out += static_cast<char>(0x80 | 126);
                out += static_cast<char>((size >> 8) & 0xFF);
                out += static_cast<char>(size & 0xFF);
            } else {
                out += static_cast<char>(0x80 | 127);
                for (int i = 7; i >= 0; --i)
                    out += static_cast<char>((static_cast<u64>(size) >> (i * 8)) & 0xFF);
            }

            u8 key[4];
            for (int i = 0; i < 4; ++i) key[i] = static_cast<u8>((mask >> (i * 8)) & 0xFF);
            out.append(reinterpret_cast<const char*>(key), 4);
            for (std::size_t i = 0; i < size; ++i)
                out += static_cast<char>(static_cast<u8>(payload[i]) ^ key[i % 4]);
            return out;
        }

    }

    Socket::~Socket() { Close(); }

    bool Socket::Open(const std::string& url, std::string* error) {
        Close();

        std::string host, port, path;
        if (!SplitWs(url, host, port, path, error)) {
            m_State = State::Failed;
            return false;
        }

        m_Stop = false;
        m_State = State::Connecting;
        m_Worker = std::thread(&Socket::Run, this, std::move(host), std::move(port),
                               std::move(path), url);
        return true;
    }

    void Socket::Send(std::string_view text) {
        std::lock_guard lock(m_Mutex);
        m_Outbox.emplace_back(text);
    }

    void Socket::Close() {
        if (!m_Worker.joinable()) {
            m_State = m_State == State::Failed ? State::Failed : State::Closed;
            return;
        }
        m_Stop = true;
        // Waking the poll rather than waiting for its timeout: a shutdown that takes a second is a
        // shutdown someone will notice on every reload.
        if (const i64 fd = m_Fd.load(); fd != -1) ::shutdown(static_cast<Fd>(fd), kShutdownBoth);
        m_Worker.join();
        m_State = State::Closed;
    }

    void Socket::Post(Event::Kind kind, std::string text) {
        std::lock_guard lock(m_Mutex);
        m_Events.push_back({ kind, std::move(text) });
    }

    std::size_t Socket::Pump(const Handler& handler) {
        std::vector<Event> ready;
        {
            std::lock_guard lock(m_Mutex);
            ready.swap(m_Events);
        }
        // Outside the lock: a handler will send on the socket it is being told about, and one that
        // deadlocks the thing that called it is a trap nobody finds twice.
        for (const Event& event : ready) if (handler) handler(event);
        return ready.size();
    }

    std::size_t Socket::Waiting() const {
        std::lock_guard lock(m_Mutex);
        return m_Events.size();
    }

    void Socket::Run(std::string host, std::string port, std::string path, std::string origin) {
        const Fd fd = Connect(host, port);
        if (!Valid(fd)) {
            m_State = State::Failed;
            Post(Event::Kind::Failed, "cannot reach " + host + ":" + port);
            return;
        }
        m_Fd = static_cast<i64>(fd);

        std::mt19937_64 noise{ std::random_device{}() };
        u8 nonce[16];
        for (u8& byte : nonce) byte = static_cast<u8>(noise() & 0xFF);
        const std::string key = Base64(nonce, sizeof nonce);

        std::string request;
        request += "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + port + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";
        (void)origin;

        const auto fail = [&](std::string why) {
            m_State = State::Failed;
            CloseFd(fd);
            m_Fd = -1;
            Post(Event::Kind::Failed, std::move(why));
        };

        if (!SendAll(fd, request.data(), request.size())) { fail("the handshake never went out"); return; }

        // The answer, up to the blank line. Anything after it is the first frame, so it is kept.
        std::string inbox;
        char chunk[2048];
        while (inbox.find("\r\n\r\n") == std::string::npos) {
            const auto got = ::recv(fd, chunk, sizeof chunk, 0);
            if (got <= 0) { fail("the server hung up during the handshake"); return; }
            inbox.append(chunk, static_cast<std::size_t>(got));
            if (inbox.size() > 64 * 1024) { fail("the server's answer is not a handshake"); return; }
        }

        const std::size_t headerEnd = inbox.find("\r\n\r\n") + 4;
        const std::string header = inbox.substr(0, headerEnd);
        inbox.erase(0, headerEnd);

        if (header.find(" 101") == std::string::npos) {
            fail("the server refused the upgrade: " + header.substr(0, header.find("\r\n")));
            return;
        }

        // The accept token proves the answer came from something that speaks the protocol, rather
        // than from a proxy that echoed a 101 at us.
        if (header.find(AcceptToken(key)) == std::string::npos) {
            fail("the server's accept token does not match the key it was given");
            return;
        }

        m_State = State::Open;
        Post(Event::Kind::Opened, "");

        std::string message;          // reassembled across continuation frames
        u8 messageOpcode = 0;
        bool closing = false;

        while (!m_Stop) {
            // Anything queued goes out first: a send made from inside a handler should not wait for
            // the next thing the server says.
            std::vector<std::string> outgoing;
            {
                std::lock_guard lock(m_Mutex);
                outgoing.swap(m_Outbox);
            }
            for (const std::string& text : outgoing) {
                const std::string frame = Frame(kText, text, static_cast<u32>(noise()));
                if (!SendAll(fd, frame.data(), frame.size())) { closing = true; break; }
            }
            if (closing) break;

            pollfd waiting{ fd, POLLIN, 0 };
            const int ready = Poll(&waiting, 1, 25);
            if (ready < 0) break;
            if (ready > 0) {
                const auto got = ::recv(fd, chunk, sizeof chunk, 0);
                if (got <= 0) break;
                inbox.append(chunk, static_cast<std::size_t>(got));
            }

            // As many whole frames as have arrived. A partial one waits for the next read rather
            // than being guessed at.
            while (inbox.size() >= 2) {
                const auto* bytes = reinterpret_cast<const u8*>(inbox.data());
                const bool final = (bytes[0] & 0x80) != 0;
                const u8 opcode = bytes[0] & 0x0F;
                const bool masked = (bytes[1] & 0x80) != 0;
                u64 size = bytes[1] & 0x7F;
                std::size_t at = 2;

                if (size == 126) {
                    if (inbox.size() < at + 2) break;
                    size = static_cast<u64>(bytes[2]) << 8 | bytes[3];
                    at += 2;
                } else if (size == 127) {
                    if (inbox.size() < at + 8) break;
                    size = 0;
                    for (int i = 0; i < 8; ++i) size = (size << 8) | bytes[at + i];
                    at += 8;
                }
                u8 key4[4]{};
                if (masked) {
                    if (inbox.size() < at + 4) break;
                    std::memcpy(key4, bytes + at, 4);
                    at += 4;
                }
                if (inbox.size() < at + size) break;

                std::string payload = inbox.substr(at, static_cast<std::size_t>(size));
                if (masked)
                    for (std::size_t i = 0; i < payload.size(); ++i)
                        payload[i] = static_cast<char>(static_cast<u8>(payload[i]) ^ key4[i % 4]);
                inbox.erase(0, at + static_cast<std::size_t>(size));

                if (opcode == kPing) {
                    const std::string pong = Frame(kPong, payload, static_cast<u32>(noise()));
                    SendAll(fd, pong.data(), pong.size());
                    continue;
                }
                if (opcode == kPong) continue;
                if (opcode == kClose) { closing = true; break; }

                if (opcode == kText || opcode == kBinary) {
                    message = std::move(payload);
                    messageOpcode = opcode;
                } else {
                    message += payload;          // a continuation of the one before it
                }
                if (final && messageOpcode != 0) {
                    Post(Event::Kind::Message, std::move(message));
                    message.clear();
                    messageOpcode = 0;
                }
            }
            if (closing) break;
        }

        // A close frame on the way out, best effort: a server that is told is a server that can
        // clean up, and one that is not has to wait for a timeout.
        const std::string goodbye = Frame(kClose, "", static_cast<u32>(noise()));
        SendAll(fd, goodbye.data(), goodbye.size());
        CloseFd(fd);
        m_Fd = -1;
        if (m_State.load() == State::Open) {
            m_State = State::Closed;
            Post(Event::Kind::Closed, "");
        }
    }

}
