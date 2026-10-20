#ifndef RUDP_PLATFORM_H
#define RUDP_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>

    typedef SOCKET socket_t;
    typedef int socklen_t;
    #define RUDP_INVALID_SOCKET INVALID_SOCKET
    #define RUDP_SOCKET_ERROR   SOCKET_ERROR
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/time.h>
    #include <time.h>
    #include <errno.h>

    typedef int socket_t;
    #define RUDP_INVALID_SOCKET (-1)
    #define RUDP_SOCKET_ERROR   (-1)
#endif

/**
 * @brief Initializes platform networking subsystem (e.g. WSAStartup on Windows).
 * @return 0 on success, negative value on failure.
 */
int rudp_platform_init(void);

/**
 * @brief Cleans up platform networking subsystem.
 */
void rudp_platform_cleanup(void);

/**
 * @brief Closes a network socket portably.
 * @param sock Socket descriptor.
 * @return 0 on success, negative value on error.
 */
int rudp_close_socket(socket_t sock);

/**
 * @brief Sets socket non-blocking mode.
 * @param sock Socket descriptor.
 * @param nonblocking true for non-blocking, false for blocking.
 * @return 0 on success, negative value on error.
 */
int rudp_set_nonblocking(socket_t sock, bool nonblocking);

/**
 * @brief Monotonic high-resolution timestamp in milliseconds.
 * @return Monotonic time in milliseconds.
 */
uint64_t rudp_get_time_ms(void);

/**
 * @brief Portable thread sleep in milliseconds.
 * @param ms Milliseconds to sleep.
 */
void rudp_sleep_ms(uint32_t ms);

/**
 * @brief Retrieves the last socket error code.
 * @return Integer error code.
 */
int rudp_get_last_socket_error(void);

#endif /* RUDP_PLATFORM_H */
