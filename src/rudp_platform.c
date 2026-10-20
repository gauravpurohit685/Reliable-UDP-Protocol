#include "rudp_platform.h"
#include <stdio.h>

#ifdef _WIN32
static bool s_winsock_initialized = false;
#endif

int rudp_platform_init(void) {
#ifdef _WIN32
    if (!s_winsock_initialized) {
        WSADATA wsa;
        int res = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (res != 0) {
            fprintf(stderr, "[RUDP-PLATFORM] WSAStartup failed: %d\n", res);
            return -1;
        }
        s_winsock_initialized = true;
    }
#endif
    return 0;
}

void rudp_platform_cleanup(void) {
#ifdef _WIN32
    if (s_winsock_initialized) {
        WSACleanup();
        s_winsock_initialized = false;
    }
#endif
}

int rudp_close_socket(socket_t sock) {
    if (sock == RUDP_INVALID_SOCKET) {
        return 0;
    }
#ifdef _WIN32
    return closesocket(sock);
#else
    return close(sock);
#endif
}

int rudp_set_nonblocking(socket_t sock, bool nonblocking) {
#ifdef _WIN32
    u_long mode = nonblocking ? 1 : 0;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(sock, F_SETFL, flags);
#endif
}

uint64_t rudp_get_time_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static bool freq_init = false;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = true;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

void rudp_sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

int rudp_get_last_socket_error(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
