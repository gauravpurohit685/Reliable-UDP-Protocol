#ifndef RUDP_SOCKET_H
#define RUDP_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rudp_platform.h"
#include "rudp_packet.h"
#include "rudp_window.h"

/* Protocol State Machine */
typedef enum {
    RUDP_STATE_CLOSED,
    RUDP_STATE_LISTEN,
    RUDP_STATE_SYN_SENT,
    RUDP_STATE_SYN_RECEIVED,
    RUDP_STATE_ESTABLISHED,
    RUDP_STATE_FIN_WAIT,
    RUDP_STATE_CLOSE_WAIT,
    RUDP_STATE_TIME_WAIT
} rudp_state_t;

/* Telemetry & Performance Statistics */
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_retransmitted;
    uint32_t fast_retransmissions;
    uint32_t duplicate_acks_received;
    uint32_t duplicate_packets_received;
    uint32_t checksum_errors;
    uint32_t syn_retries;
    uint32_t fin_retries;
} rudp_stats_t;

/* Stream Byte Buffer for Reassembled In-Order Payload */
#define RUDP_REASSEMBLY_BUF_SIZE (1024 * 1024) /* 1 MB circular reassembly buffer */

typedef struct {
    uint8_t buffer[RUDP_REASSEMBLY_BUF_SIZE];
    size_t  head; /* Read pointer */
    size_t  tail; /* Write pointer */
    size_t  count; /* Current bytes in buffer */
} rudp_byte_fifo_t;

/* RUDP Socket Protocol Control Block (PCB) */
typedef struct rudp_socket {
    socket_t            raw_fd;
    struct sockaddr_in  local_addr;
    struct sockaddr_in  remote_addr;
    bool                has_remote;

    rudp_state_t        state;
    uint32_t            local_isn;
    uint32_t            remote_isn;

    rudp_send_window_t  send_win;
    rudp_recv_window_t  recv_win;

    rudp_byte_fifo_t    rx_stream;
    bool                peer_fin_received;

    uint32_t            rto_ms;
    uint32_t            handshake_timeout_ms;

    rudp_stats_t        stats;
} rudp_socket_t;

/**
 * @brief Allocates and initializes an RUDP socket.
 * @return Pointer to allocated rudp_socket_t, or NULL on failure.
 */
rudp_socket_t* rudp_socket(void);

/**
 * @brief Binds RUDP socket to local IP and port.
 * @param sock Socket handle.
 * @param ip Local IP string (e.g. "0.0.0.0" or "127.0.0.1").
 * @param port Local port in host byte order.
 * @return 0 on success, negative on error.
 */
int rudp_bind(rudp_socket_t *sock, const char *ip, uint16_t port);

/**
 * @brief Transitions socket to passive listening state.
 * @param sock Socket handle.
 * @return 0 on success, negative on error.
 */
int rudp_listen(rudp_socket_t *sock);

/**
 * @brief Waits for and accepts an incoming 3-way handshake connection.
 * @param sock Listening socket handle.
 * @param timeout_ms Maximum time to wait in milliseconds (-1 for indefinite).
 * @return New connected rudp_socket_t on success, NULL on timeout/error.
 */
rudp_socket_t* rudp_accept(rudp_socket_t *sock, int timeout_ms);

/**
 * @brief Initiates 3-way handshake to connect to remote server.
 * @param sock Socket handle.
 * @param remote_ip Remote IP string.
 * @param remote_port Remote port in host byte order.
 * @return 0 on successful establishment, negative on failure/timeout.
 */
int rudp_connect(rudp_socket_t *sock, const char *remote_ip, uint16_t remote_port);

/**
 * @brief Reliably sends application data over RUDP connection.
 * Blocks until all bytes are queued into sliding window and acknowledged.
 *
 * @param sock Connected socket handle.
 * @param buf Data buffer to transmit.
 * @param len Length of data in bytes.
 * @return Number of bytes successfully transmitted, or negative on fatal error.
 */
int rudp_send(rudp_socket_t *sock, const void *buf, size_t len);

/**
 * @brief Receives in-order application data from RUDP stream.
 *
 * @param sock Connected socket handle.
 * @param buf Destination buffer.
 * @param max_len Maximum bytes to read.
 * @param timeout_ms Timeout in milliseconds (-1 to block until data arrives).
 * @return Number of bytes read, 0 if remote closed gracefully (FIN), negative on error/timeout.
 */
int rudp_recv(rudp_socket_t *sock, void *buf, size_t max_len, int timeout_ms);

/**
 * @brief Gracefully terminates RUDP connection via FIN handshake and frees resources.
 * @param sock Socket handle to close.
 * @return 0 on success, negative on error.
 */
int rudp_close(rudp_socket_t *sock);

/**
 * @brief Retrieves telemetry and reliability statistics for the socket.
 * @param sock Socket handle.
 * @param[out] stats Destination stats structure.
 */
void rudp_get_stats(const rudp_socket_t *sock, rudp_stats_t *stats);

#endif /* RUDP_SOCKET_H */
