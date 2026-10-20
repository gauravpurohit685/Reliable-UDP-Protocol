#ifndef RUDP_WINDOW_H
#define RUDP_WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rudp_packet.h"

#define RUDP_DEFAULT_WINDOW_SIZE     16
#define RUDP_MAX_WINDOW_SIZE         64
#define RUDP_DEFAULT_RTO_MS          200
#define RUDP_MIN_RTO_MS              50
#define RUDP_MAX_RTO_MS              2000
#define RUDP_MAX_RETRANSMISSIONS     10
#define RUDP_FAST_RETRANSMIT_THRESH  3

/* Sender window slot tracking a packet in flight */
typedef struct {
    rudp_packet_t packet;
    uint64_t      last_sent_ms;
    uint32_t      retransmit_count;
    bool          in_flight;
    bool          acknowledged;
} rudp_send_slot_t;

/* Sender Sliding Window */
typedef struct {
    uint32_t         base_seq;       /* Sequence number of oldest unacknowledged packet */
    uint32_t         next_seq;       /* Next sequence number to be assigned */
    uint32_t         window_size;    /* Configured sender window size */
    uint32_t         peer_win_size;  /* Advertised window from receiver */
    uint32_t         rto_ms;         /* Retransmission timeout in ms */
    uint32_t         dup_ack_count;  /* Consecutive duplicate ACKs for base_seq */
    uint32_t         last_ack_seen;  /* Highest ACK seen */
    rudp_send_slot_t slots[RUDP_MAX_WINDOW_SIZE];
} rudp_send_window_t;

/* Receiver window slot tracking out-of-order received packets */
typedef struct {
    rudp_packet_t packet;
    bool          received;
} rudp_recv_slot_t;

/* Receiver Sliding Window (Selective Repeat Reassembly) */
typedef struct {
    uint32_t         expected_seq;   /* Next in-order sequence number expected */
    uint32_t         window_size;    /* Receiver window capacity */
    rudp_recv_slot_t slots[RUDP_MAX_WINDOW_SIZE];
} rudp_recv_window_t;

/**
 * @brief Initializes sender sliding window.
 * @param win Pointer to sender window structure.
 * @param initial_seq Starting sequence number (e.g. from handshake).
 * @param window_size Maximum packets in flight.
 * @param rto_ms Retransmission timeout in milliseconds.
 */
void rudp_send_window_init(rudp_send_window_t *win,
                           uint32_t initial_seq,
                           uint32_t window_size,
                           uint32_t rto_ms);

/**
 * @brief Checks if sender window has room to transmit a new packet.
 * @param win Sender window.
 * @return true if room is available, false if window is full.
 */
bool rudp_send_window_can_send(const rudp_send_window_t *win);

/**
 * @brief Pushes a new data packet into the sender window.
 * @param win Sender window.
 * @param pkt Packet to queue and transmit.
 * @return Assigned sequence number on success, 0 on failure.
 */
uint32_t rudp_send_window_push(rudp_send_window_t *win, const rudp_packet_t *pkt);

/**
 * @brief Processes an incoming ACK in the sender window.
 * @param win Sender window.
 * @param ack_num Acknowledged sequence number.
 * @param peer_window Advertised receive window from peer.
 * @param[out] out_fast_retransmit Set to true if fast retransmit is triggered.
 * @return Number of packets successfully acknowledged/slid.
 */
int rudp_send_window_on_ack(rudp_send_window_t *win,
                            uint32_t ack_num,
                            uint8_t peer_window,
                            bool *out_fast_retransmit);

/**
 * @brief Finds the next packet in the sender window requiring retransmission due to timeout.
 * @param win Sender window.
 * @param current_time_ms Current monotonic timestamp in ms.
 * @param[out] out_pkt Pointer to copy the retransmitted packet into.
 * @param[out] out_expired True if a packet exceeded RUDP_MAX_RETRANSMISSIONS.
 * @return true if a packet was selected for retransmission, false otherwise.
 */
bool rudp_send_window_get_retransmission(rudp_send_window_t *win,
                                         uint64_t current_time_ms,
                                         rudp_packet_t *out_pkt,
                                         bool *out_expired);

/**
 * @brief Checks if all sent packets in the window have been acknowledged.
 * @param win Sender window.
 * @return true if window is completely empty (all ACKed), false otherwise.
 */
bool rudp_send_window_is_empty(const rudp_send_window_t *win);

/**
 * @brief Initializes receiver sliding window.
 * @param win Pointer to receiver window structure.
 * @param initial_seq Expected starting sequence number.
 * @param window_size Receiver buffer window capacity.
 */
void rudp_recv_window_init(rudp_recv_window_t *win,
                           uint32_t initial_seq,
                           uint32_t window_size);

/**
 * @brief Processes an incoming packet in the receiver window.
 *
 * @param win Receiver window.
 * @param pkt Incoming packet.
 * @param[out] out_is_duplicate Set to true if packet was a duplicate.
 * @param[out] out_ack_to_send Sequence number that should be ACKed.
 * @return 0 on success, negative on out-of-window discard.
 */
int rudp_recv_window_on_packet(rudp_recv_window_t *win,
                               const rudp_packet_t *pkt,
                               bool *out_is_duplicate,
                               uint32_t *out_ack_to_send);

/**
 * @brief Drains the next in-order packet from the reassembly buffer if available.
 * @param win Receiver window.
 * @param[out] out_pkt Destination buffer for in-order packet.
 * @return true if an in-order packet was retrieved, false if awaiting sequence.
 */
bool rudp_recv_window_pop_in_order(rudp_recv_window_t *win, rudp_packet_t *out_pkt);

#endif /* RUDP_WINDOW_H */
