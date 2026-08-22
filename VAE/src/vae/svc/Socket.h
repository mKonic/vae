#pragma once

#include "vae/base/Base.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vae::svc {

    // The token a server must answer a handshake with: base64(SHA-1(key + the protocol's GUID)).
    // Exposed because a test needs a server to talk to, and a test server that computes this a
    // second way is a test of two SHA-1s rather than of the handshake.
    std::string AcceptToken(std::string_view key);

    // A WebSocket, because a live view needs the server to speak first and `Http` can only ask.
    //
    // Same contract as the HTTP client and for the same reason: the socket runs on a thread of its
    // own, and everything it produces waits in a queue until `Pump` is called on the thread the
    // scripts run on. A script never sees a worker and never needs a lock.
    //
    // `ws://` only. `wss://` is refused with a message rather than silently downgraded — a socket
    // that says it is encrypted and is not is worse than one that says it cannot.
    class Socket {
    public:
        enum class State : u8 { Closed, Connecting, Open, Failed };

        struct Event {
            enum class Kind : u8 { Opened, Message, Closed, Failed } kind = Kind::Message;
            std::string text;      // the message, or the reason it closed or failed
        };
        using Handler = std::function<void(const Event&)>;

        Socket() = default;
        ~Socket();
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;

        // Starts connecting. Returns false only for a url this cannot possibly serve; everything
        // else is reported as a Failed event, because a connection that fails after the call has
        // returned has to arrive the same way as one that fails during it.
        bool Open(const std::string& url, std::string* error = nullptr);
        void Send(std::string_view text);
        void Close();

        State Status() const { return m_State.load(); }
        bool Connected() const { return m_State.load() == State::Open; }

        // Hands over everything that has arrived, in order. Returns how many events were delivered.
        std::size_t Pump(const Handler& handler);
        std::size_t Waiting() const;

    private:
        void Run(std::string host, std::string port, std::string path, std::string origin);
        void Post(Event::Kind kind, std::string text);

        std::thread m_Worker;
        std::atomic<State> m_State{ State::Closed };
        std::atomic<bool> m_Stop{ false };
        // The socket handle, widened so the header does not have to know what a socket is called
        // here: an int on POSIX, a UINT_PTR on Windows, -1 for "none" on both.
        std::atomic<i64> m_Fd{ -1 };

        mutable std::mutex m_Mutex;
        std::vector<Event> m_Events;
        std::vector<std::string> m_Outbox;
    };

}
