#include "rudp_window.h"
#include <string.h>

void rudp_send_window_init(rudp_send_window_t *win,
                           uint32_t initial_seq,
                           uint32_t window_size,
                           uint32_t rto_ms) {
    if (!win) return;
    memset(win, 0, sizeof(*win));
    win->base_seq = initial_seq;
    win->next_seq = initial_seq;
    win->window_size = (window_size > 0 && window_size <= RUDP_MAX_WINDOW_SIZE)
                       ? window_size : RUDP_DEFAULT_WINDOW_SIZE;
    win->peer_win_size = win->window_size;
    win->rto_ms = (rto_ms > 0) ? rto_ms : RUDP_DEFAULT_RTO_MS;
    win->dup_ack_count = 0;
    win->last_ack_seen = initial_seq > 0 ? (initial_seq - 1) : 0;
}

bool rudp_send_window_can_send(const rudp_send_window_t *win) {
    if (!win) return false;
    uint32_t effective_window = win->window_size;
    if (win->peer_win_size > 0 && win->peer_win_size < effective_window) {
        effective_window = win->peer_win_size;
    }
    return ((win->next_seq - win->base_seq) < effective_window);
}

uint32_t rudp_send_window_push(rudp_send_window_t *win, const rudp_packet_t *pkt) {
    if (!win || !pkt) return 0;
    if (!rudp_send_window_can_send(win)) {
        return 0;
    }

    uint32_t assigned_seq = win->next_seq;
    uint32_t slot_idx = assigned_seq % RUDP_MAX_WINDOW_SIZE;

    rudp_send_slot_t *slot = &win->slots[slot_idx];
    slot->packet = *pkt;
    slot->packet.header.seq_num = assigned_seq;
    slot->last_sent_ms = rudp_get_time_ms();
    slot->retransmit_count = 0;
    slot->in_flight = true;
    slot->acknowledged = false;

    win->next_seq++;
    return assigned_seq;
}

int rudp_send_window_on_ack(rudp_send_window_t *win,
                            uint32_t ack_num,
                            uint8_t peer_window,
                            bool *out_fast_retransmit) {
    if (!win) return 0;
    if (out_fast_retransmit) *out_fast_retransmit = false;

    if (peer_window > 0) {
        win->peer_win_size = (peer_window <= RUDP_MAX_WINDOW_SIZE) ? peer_window : RUDP_MAX_WINDOW_SIZE;
    }

    /* Valid ACK in window */
    if (ack_num >= win->base_seq && ack_num < win->next_seq) {
        uint32_t slot_idx = ack_num % RUDP_MAX_WINDOW_SIZE;
        rudp_send_slot_t *slot = &win->slots[slot_idx];

        if (!slot->acknowledged) {
            slot->acknowledged = true;
            slot->in_flight = false;
        }

        /* Slide cumulative base forward */
        int slid_count = 0;
        while (win->base_seq < win->next_seq &&
               win->slots[win->base_seq % RUDP_MAX_WINDOW_SIZE].acknowledged) {
            rudp_send_slot_t *head = &win->slots[win->base_seq % RUDP_MAX_WINDOW_SIZE];
            head->acknowledged = false;
            head->in_flight = false;
            win->base_seq++;
            slid_count++;
        }

        win->dup_ack_count = 0;
        win->last_ack_seen = ack_num;
        return slid_count;
    }

    /* Duplicate ACK for an older packet */
    if (ack_num < win->base_seq) {
        win->dup_ack_count++;
        if (win->dup_ack_count >= RUDP_FAST_RETRANSMIT_THRESH) {
            if (out_fast_retransmit) {
                *out_fast_retransmit = true;
            }
            win->dup_ack_count = 0;
        }
    }

    return 0;
}

bool rudp_send_window_get_retransmission(rudp_send_window_t *win,
                                         uint64_t current_time_ms,
                                         rudp_packet_t *out_pkt,
                                         bool *out_expired) {
    if (!win || !out_pkt) return false;
    if (out_expired) *out_expired = false;

    for (uint32_t seq = win->base_seq; seq < win->next_seq; ++seq) {
        uint32_t slot_idx = seq % RUDP_MAX_WINDOW_SIZE;
        rudp_send_slot_t *slot = &win->slots[slot_idx];

        if (slot->in_flight && !slot->acknowledged) {
            /* Exponential backoff: RTO * 2^(retries - 1) up to 8x */
            uint32_t backoff_mult = 1;
            if (slot->retransmit_count > 0) {
                uint32_t shift = (slot->retransmit_count > 3) ? 3 : slot->retransmit_count;
                backoff_mult = 1U << shift;
            }
            uint32_t effective_rto = win->rto_ms * backoff_mult;
            if (effective_rto > RUDP_MAX_RTO_MS) {
                effective_rto = RUDP_MAX_RTO_MS;
            }

            if (current_time_ms >= slot->last_sent_ms + effective_rto) {
                if (slot->retransmit_count >= RUDP_MAX_RETRANSMISSIONS) {
                    if (out_expired) *out_expired = true;
                    return false;
                }

                slot->retransmit_count++;
                slot->last_sent_ms = current_time_ms;
                *out_pkt = slot->packet;
                return true;
            }
        }
    }

    return false;
}

bool rudp_send_window_is_empty(const rudp_send_window_t *win) {
    if (!win) return true;
    return (win->base_seq >= win->next_seq);
}

void rudp_recv_window_init(rudp_recv_window_t *win,
                           uint32_t initial_seq,
                           uint32_t window_size) {
    if (!win) return;
    memset(win, 0, sizeof(*win));
    win->expected_seq = initial_seq;
    win->window_size = (window_size > 0 && window_size <= RUDP_MAX_WINDOW_SIZE)
                       ? window_size : RUDP_DEFAULT_WINDOW_SIZE;
}

int rudp_recv_window_on_packet(rudp_recv_window_t *win,
                               const rudp_packet_t *pkt,
                               bool *out_is_duplicate,
                               uint32_t *out_ack_to_send) {
    if (!win || !pkt || !out_is_duplicate || !out_ack_to_send) {
        return -1;
    }

    uint32_t seq = pkt->header.seq_num;
    *out_ack_to_send = seq;

    /* Duplicate from previously received/delivered sequence range */
    if (seq < win->expected_seq) {
        *out_is_duplicate = true;
        return 0; /* ACK still sent to assist sender convergence */
    }

    /* Within active receiver sliding window */
    if (seq < win->expected_seq + win->window_size) {
        uint32_t slot_idx = seq % RUDP_MAX_WINDOW_SIZE;
        rudp_recv_slot_t *slot = &win->slots[slot_idx];

        if (slot->received && slot->packet.header.seq_num == seq) {
            *out_is_duplicate = true;
            return 0;
        }

        slot->packet = *pkt;
        slot->received = true;
        *out_is_duplicate = false;
        return 0;
    }

    /* Ahead of receive window: dropped */
    *out_is_duplicate = false;
    return -2;
}

bool rudp_recv_window_pop_in_order(rudp_recv_window_t *win, rudp_packet_t *out_pkt) {
    if (!win || !out_pkt) return false;

    uint32_t slot_idx = win->expected_seq % RUDP_MAX_WINDOW_SIZE;
    rudp_recv_slot_t *slot = &win->slots[slot_idx];

    if (slot->received && slot->packet.header.seq_num == win->expected_seq) {
        *out_pkt = slot->packet;
        slot->received = false;
        win->expected_seq++;
        return true;
    }

    return false;
}
