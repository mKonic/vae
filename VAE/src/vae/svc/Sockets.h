#pragma once

#include "vae/base/Base.h"

// BSD sockets and Winsock, spelled the same way.
//
// Not an abstraction — the two APIs are the same API with four names changed and one initialiser
// added, and wrapping them in classes would hide that. What is here is the four names, so the code
// that actually talks the protocol reads as one thing on both systems.
//
// The Windows half is written and has never been compiled; see design/windows.md.

#ifdef VAE_PLATFORM_WINDOWS

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>

namespace vae::svc {

    using Fd = SOCKET;
    inline constexpr Fd kNoFd = INVALID_SOCKET;
    inline bool Valid(Fd fd) { return fd != INVALID_SOCKET; }

    inline int CloseFd(Fd fd) { return ::closesocket(fd); }
    inline int Poll(pollfd* fds, unsigned long count, int millis) {
        return ::WSAPoll(fds, count, millis);
    }
    // Winsock has no MSG_NOSIGNAL because Windows has no SIGPIPE: a send to a closed socket is an
    // error return, which is what the callers already handle.
    inline constexpr int kNoSignal = 0;
    inline constexpr int kShutdownBoth = SD_BOTH;

    // Winsock is the one library that has to be started before it can be used, and starting it
    // twice is fine as long as it is stopped as many times. Once per process, on first use.
    struct Startup {
        Startup() { WSADATA data{}; ::WSAStartup(MAKEWORD(2, 2), &data); }
        ~Startup() { ::WSACleanup(); }
    };
    inline void EnsureStarted() { static Startup once; }

}

#else

    #include <arpa/inet.h>
    #include <netdb.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>

namespace vae::svc {

    using Fd = int;
    inline constexpr Fd kNoFd = -1;
    inline bool Valid(Fd fd) { return fd >= 0; }

    inline int CloseFd(Fd fd) { return ::close(fd); }
    inline int Poll(pollfd* fds, nfds_t count, int millis) { return ::poll(fds, count, millis); }

    inline constexpr int kNoSignal = MSG_NOSIGNAL;
    inline constexpr int kShutdownBoth = SHUT_RDWR;

    inline void EnsureStarted() {}

}

#endif
