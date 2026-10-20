#include "rudp_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FIFO Stream Buffer Helpers */
static size_t rudp_fifo_write(rudp_byte_fifo_t *fifo, const uint8_t *data, size_t len) {
    if (!fifo || !data || len == 0) return 0;
    size_t space = RUDP_REASSEMBLY_BUF_SIZE - fifo->count;
    size_t to_write = (len < space) ? len : space;

    for (size_t i = 0; i < to_write; ++i) {
        fifo->buffer[fifo->tail] = data[i];
        fifo->tail = (fifo->tail + 1) % RUDP_REASSEMBLY_BUF_SIZE;
    }
    fifo->count += to_write;
    return to_write;
}

static size_t rudp_fifo_read(rudp_byte_fifo_t *fifo, uint8_t *data, size_t max_len) {
    if (!fifo || !data || max_len == 0) return 0;
    size_t to_read = (fifo->count < max_len) ? fifo->count : max_len;

    for (size_t i = 0; i < to_read; ++i) {
        data[i] = fifo->buffer[fifo->head];
        fifo->head = (fifo->head + 1) % RUDP_REASSEMBLY_BUF_SIZE;
    }
    fifo->count -= to_read;
    return to_read;
}

/* Transmit raw wire packet to socket remote address */
static int rudp_send_raw_packet(rudp_socket_t *sock, const rudp_packet_t *pkt) {
    if (!sock || !pkt || !sock->has_remote) return -1;

    uint8_t wire_buf[RUDP_MAX_PACKET_SIZE];
    size_t wire_len = 0;

    rudp_status_t status = rudp_pack(pkt, wire_buf, sizeof(wire_buf), &wire_len);
    if (status != RUDP_SUCCESS) {
        return -1;
    }

    int sent = sendto(sock->raw_fd,
                      (const char*)wire_buf,
                      (int)wire_len,
                      0,
                      (struct sockaddr*)&sock->remote_addr,
                      sizeof(sock->remote_addr));
    return (sent == (int)wire_len) ? 0 : -1;
}

/* Internal driver: handles socket readability, ACK generation, in-order reassembly, and retransmission */
static int rudp_drive_io(rudp_socket_t *sock, int timeout_ms) {
    if (!sock) return -1;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock->raw_fd, &read_fds);

    struct timeval tv;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    } else {
        tv.tv_sec = 0;
        tv.tv_usec = 10000; /* 10ms default quantum */
    }

    int sel = select((int)sock->raw_fd + 1, &read_fds, NULL, NULL, &tv);

    /* 1. Inbound datagram available */
    if (sel > 0 && FD_ISSET(sock->raw_fd, &read_fds)) {
        uint8_t wire_buf[RUDP_MAX_PACKET_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int recvd = recvfrom(sock->raw_fd,
                             (char*)wire_buf,
                             (int)sizeof(wire_buf),
                             0,
                             (struct sockaddr*)&from_addr,
                             &from_len);
        if (recvd > 0) {
            rudp_packet_t in_pkt;
            rudp_status_t status = rudp_unpack(wire_buf, (size_t)recvd, &in_pkt);
            if (status == RUDP_ERR_CHECKSUM) {
                sock->stats.checksum_errors++;
            } else if (status == RUDP_SUCCESS) {
                sock->stats.packets_received++;

                /* Auto-learn remote endpoint if not yet bound */
                if (!sock->has_remote) {
                    sock->remote_addr = from_addr;
                    sock->has_remote = true;
                }

                /* Handle ACK flag */
                if (in_pkt.header.flags & RUDP_FLAG_ACK) {
                    bool fast_retransmit = false;
                    rudp_send_window_on_ack(&sock->send_win,
                                            in_pkt.header.ack_num,
                                            in_pkt.header.window_size,
                                            &fast_retransmit);
                    if (fast_retransmit) {
                        sock->stats.fast_retransmissions++;
                        /* Immediately retransmit window base packet */
                        uint32_t base_idx = sock->send_win.base_seq % RUDP_MAX_WINDOW_SIZE;
                        if (sock->send_win.slots[base_idx].in_flight) {
                            rudp_send_raw_packet(sock, &sock->send_win.slots[base_idx].packet);
                            sock->send_win.slots[base_idx].last_sent_ms = rudp_get_time_ms();
                            sock->send_win.slots[base_idx].retransmit_count++;
                            sock->stats.packets_retransmitted++;
                        }
                    }
                }

                /* Handle DATA flag */
                if (in_pkt.header.flags & RUDP_FLAG_DATA) {
                    bool is_duplicate = false;
                    uint32_t ack_seq = 0;
                    int win_res = rudp_recv_window_on_packet(&sock->recv_win,
                                                             &in_pkt,
                                                             &is_duplicate,
                                                             &ack_seq);
                    if (win_res == 0) {
                        if (is_duplicate) {
                            sock->stats.duplicate_packets_received++;
                        }

                        /* Send ACK for received packet */
                        rudp_packet_t ack_pkt;
                        memset(&ack_pkt, 0, sizeof(ack_pkt));
                        ack_pkt.header.seq_num = 0;
                        ack_pkt.header.ack_num = ack_seq;
                        ack_pkt.header.flags = RUDP_FLAG_ACK;
                        ack_pkt.header.window_size = RUDP_DEFAULT_WINDOW_SIZE;
                        ack_pkt.header.payload_len = 0;

                        rudp_send_raw_packet(sock, &ack_pkt);
                        sock->stats.packets_sent++;

                        /* Drain consecutive in-order packets to stream FIFO */
                        rudp_packet_t delivered;
                        while (rudp_recv_window_pop_in_order(&sock->recv_win, &delivered)) {
                            if (delivered.header.payload_len > 0) {
                                rudp_fifo_write(&sock->rx_stream,
                                                delivered.payload,
                                                delivered.header.payload_len);
                                sock->stats.bytes_received += delivered.header.payload_len;
                            }
                        }
                    }
                }

                /* Handle FIN flag */
                if (in_pkt.header.flags & RUDP_FLAG_FIN) {
                    sock->peer_fin_received = true;

                    /* Send FIN-ACK */
                    rudp_packet_t fin_ack;
                    memset(&fin_ack, 0, sizeof(fin_ack));
                    fin_ack.header.seq_num = 0;
                    fin_ack.header.ack_num = in_pkt.header.seq_num + 1;
                    fin_ack.header.flags = RUDP_FLAG_ACK;
                    fin_ack.header.window_size = RUDP_DEFAULT_WINDOW_SIZE;
                    fin_ack.header.payload_len = 0;

                    rudp_send_raw_packet(sock, &fin_ack);
                    sock->stats.packets_sent++;
                }

                /* Handle RST flag */
                if (in_pkt.header.flags & RUDP_FLAG_RST) {
                    sock->state = RUDP_STATE_CLOSED;
                    return -1;
                }
            }
        }
    }

    /* 2. Check for retransmissions due to timeout */
    uint64_t now = rudp_get_time_ms();
    rudp_packet_t retrans_pkt;
    bool expired = false;

    while (rudp_send_window_get_retransmission(&sock->send_win, now, &retrans_pkt, &expired)) {
        rudp_send_raw_packet(sock, &retrans_pkt);
        sock->stats.packets_retransmitted++;
    }

    if (expired) {
        return -2; /* Link failure / Max retries reached */
    }

    return 0;
}

rudp_socket_t* rudp_socket(void) {
    rudp_platform_init();

    socket_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == RUDP_INVALID_SOCKET) {
        return NULL;
    }

    if (rudp_set_nonblocking(fd, true) != 0) {
        rudp_close_socket(fd);
        return NULL;
    }

    rudp_socket_t *sock = (rudp_socket_t*)calloc(1, sizeof(rudp_socket_t));
    if (!sock) {
        rudp_close_socket(fd);
        return NULL;
    }

    sock->raw_fd = fd;
    sock->state = RUDP_STATE_CLOSED;
    sock->rto_ms = RUDP_DEFAULT_RTO_MS;
    sock->handshake_timeout_ms = 5000;
    sock->local_isn = (uint32_t)(rudp_get_time_ms() & 0x7FFFFFFF);

    return sock;
}

int rudp_bind(rudp_socket_t *sock, const char *ip, uint16_t port) {
    if (!sock) return -1;

    memset(&sock->local_addr, 0, sizeof(sock->local_addr));
    sock->local_addr.sin_family = AF_INET;
    sock->local_addr.sin_port = htons(port);

    if (ip && strlen(ip) > 0 && strcmp(ip, "0.0.0.0") != 0) {
        sock->local_addr.sin_addr.s_addr = inet_addr(ip);
    } else {
        sock->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(sock->raw_fd, (struct sockaddr*)&sock->local_addr, sizeof(sock->local_addr)) != 0) {
        return -1;
    }

    return 0;
}

int rudp_listen(rudp_socket_t *sock) {
    if (!sock) return -1;
    sock->state = RUDP_STATE_LISTEN;
    return 0;
}

rudp_socket_t* rudp_accept(rudp_socket_t *sock, int timeout_ms) {
    if (!sock || sock->state != RUDP_STATE_LISTEN) return NULL;

    uint64_t start_ms = rudp_get_time_ms();
    uint8_t wire_buf[RUDP_MAX_PACKET_SIZE];

    while (timeout_ms < 0 || (rudp_get_time_ms() - start_ms) < (uint64_t)timeout_ms) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock->raw_fd, &fds);

        struct timeval tv = {0, 20000}; /* 20ms */
        int sel = select((int)sock->raw_fd + 1, &fds, NULL, NULL, &tv);

        if (sel > 0 && FD_ISSET(sock->raw_fd, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int recvd = recvfrom(sock->raw_fd,
                                 (char*)wire_buf,
                                 (int)sizeof(wire_buf),
                                 0,
                                 (struct sockaddr*)&client_addr,
                                 &client_len);
            if (recvd > 0) {
                rudp_packet_t pkt;
                if (rudp_unpack(wire_buf, (size_t)recvd, &pkt) == RUDP_SUCCESS) {
                    if (pkt.header.flags & RUDP_FLAG_SYN) {
                        /* Received SYN -> create connected socket and respond with SYN-ACK */
                        rudp_socket_t *client_sock = (rudp_socket_t*)calloc(1, sizeof(rudp_socket_t));
                        if (!client_sock) return NULL;

                        client_sock->raw_fd = sock->raw_fd; /* Shares listening UDP port */
                        client_sock->local_addr = sock->local_addr;
                        client_sock->remote_addr = client_addr;
                        client_sock->has_remote = true;
                        client_sock->remote_isn = pkt.header.seq_num;
                        client_sock->local_isn = (uint32_t)((rudp_get_time_ms() + 12345) & 0x7FFFFFFF);
                        client_sock->rto_ms = sock->rto_ms;

                        rudp_send_window_init(&client_sock->send_win,
                                              client_sock->local_isn + 1,
                                              RUDP_DEFAULT_WINDOW_SIZE,
                                              client_sock->rto_ms);
                        rudp_recv_window_init(&client_sock->recv_win,
                                              client_sock->remote_isn + 1,
                                              RUDP_DEFAULT_WINDOW_SIZE);

                        /* Send SYN-ACK */
                        rudp_packet_t syn_ack;
                        memset(&syn_ack, 0, sizeof(syn_ack));
                        syn_ack.header.seq_num = client_sock->local_isn;
                        syn_ack.header.ack_num = client_sock->remote_isn + 1;
                        syn_ack.header.flags = (RUDP_FLAG_SYN | RUDP_FLAG_ACK);
                        syn_ack.header.window_size = RUDP_DEFAULT_WINDOW_SIZE;
                        syn_ack.header.payload_len = 0;

                        rudp_send_raw_packet(client_sock, &syn_ack);
                        client_sock->stats.packets_sent++;

                        /* Wait for final client ACK to finalize handshake */
                        uint64_t ack_wait_start = rudp_get_time_ms();
                        while ((rudp_get_time_ms() - ack_wait_start) < 3000) {
                            fd_set ack_fds;
                            FD_ZERO(&ack_fds);
                            FD_SET(client_sock->raw_fd, &ack_fds);
                            struct timeval ack_tv = {0, 20000};
                            if (select((int)client_sock->raw_fd + 1, &ack_fds, NULL, NULL, &ack_tv) > 0) {
                                int ack_recvd = recvfrom(client_sock->raw_fd,
                                                         (char*)wire_buf,
                                                         (int)sizeof(wire_buf),
                                                         0,
                                                         (struct sockaddr*)&client_addr,
                                                         &client_len);
                                if (ack_recvd > 0) {
                                    rudp_packet_t ack_pkt;
                                    if (rudp_unpack(wire_buf, (size_t)ack_recvd, &ack_pkt) == RUDP_SUCCESS) {
                                        if ((ack_pkt.header.flags & RUDP_FLAG_ACK) &&
                                            ack_pkt.header.ack_num == (client_sock->local_isn + 1)) {
                                            client_sock->state = RUDP_STATE_ESTABLISHED;
                                            return client_sock;
                                        }
                                    }
                                }
                            }
                            /* Retransmit SYN-ACK if needed */
                            if ((rudp_get_time_ms() - ack_wait_start) >= 200) {
                                rudp_send_raw_packet(client_sock, &syn_ack);
                                client_sock->stats.packets_sent++;
                                client_sock->stats.syn_retries++;
                                ack_wait_start = rudp_get_time_ms();
                            }
                        }

                        /* If ACK missed, assume ESTABLISHED on incoming data */
                        client_sock->state = RUDP_STATE_ESTABLISHED;
                        return client_sock;
                    }
                }
            }
        }
    }

    return NULL;
}

int rudp_connect(rudp_socket_t *sock, const char *remote_ip, uint16_t remote_port) {
    if (!sock || !remote_ip) return -1;

    memset(&sock->remote_addr, 0, sizeof(sock->remote_addr));
    sock->remote_addr.sin_family = AF_INET;
    sock->remote_addr.sin_port = htons(remote_port);
    sock->remote_addr.sin_addr.s_addr = inet_addr(remote_ip);
    sock->has_remote = true;

    sock->local_isn = (uint32_t)(rudp_get_time_ms() & 0x7FFFFFFF);
    sock->state = RUDP_STATE_SYN_SENT;

    rudp_packet_t syn_pkt;
    memset(&syn_pkt, 0, sizeof(syn_pkt));
    syn_pkt.header.seq_num = sock->local_isn;
    syn_pkt.header.ack_num = 0;
    syn_pkt.header.flags = RUDP_FLAG_SYN;
    syn_pkt.header.window_size = RUDP_DEFAULT_WINDOW_SIZE;
    syn_pkt.header.payload_len = 0;

    uint64_t connect_start = rudp_get_time_ms();
    int retries = 0;
    const int max_retries = 8;
    uint8_t wire_buf[RUDP_MAX_PACKET_SIZE];

    while (retries < max_retries && (rudp_get_time_ms() - connect_start) < sock->handshake_timeout_ms) {
        /* Transmit SYN */
        rudp_send_raw_packet(sock, &syn_pkt);
        sock->stats.packets_sent++;
        if (retries > 0) sock->stats.syn_retries++;

        /* Wait for SYN-ACK */
        uint64_t wait_start = rudp_get_time_ms();
        while ((rudp_get_time_ms() - wait_start) < 250) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock->raw_fd, &fds);
            struct timeval tv = {0, 20000};

            int sel = select((int)sock->raw_fd + 1, &fds, NULL, NULL, &tv);
            if (sel > 0 && FD_ISSET(sock->raw_fd, &fds)) {
                struct sockaddr_in from_addr;
                socklen_t from_len = sizeof(from_addr);
                int recvd = recvfrom(sock->raw_fd,
                                     (char*)wire_buf,
                                     (int)sizeof(wire_buf),
                                     0,
                                     (struct sockaddr*)&from_addr,
                                     &from_len);
                if (recvd > 0) {
                    rudp_packet_t resp;
                    if (rudp_unpack(wire_buf, (size_t)recvd, &resp) == RUDP_SUCCESS) {
                        if ((resp.header.flags & (RUDP_FLAG_SYN | RUDP_FLAG_ACK)) &&
                            resp.header.ack_num == (sock->local_isn + 1)) {
                            sock->remote_isn = resp.header.seq_num;

                            /* Initialize Sliding Windows */
                            rudp_send_window_init(&sock->send_win,
                                                  sock->local_isn + 1,
                                                  RUDP_DEFAULT_WINDOW_SIZE,
                                                  sock->rto_ms);
                            rudp_recv_window_init(&sock->recv_win,
                                                  sock->remote_isn + 1,
                                                  RUDP_DEFAULT_WINDOW_SIZE);

                            /* Send ACK to complete 3-Way Handshake */
                            rudp_packet_t ack_pkt;
                            memset(&ack_pkt, 0, sizeof(ack_pkt));
                            ack_pkt.header.seq_num = sock->local_isn + 1;
                            ack_pkt.header.ack_num = sock->remote_isn + 1;
                            ack_pkt.header.flags = RUDP_FLAG_ACK;
                            ack_pkt.header.window_size = RUDP_DEFAULT_WINDOW_SIZE;
                            ack_pkt.header.payload_len = 0;

                            rudp_send_raw_packet(sock, &ack_pkt);
                            sock->stats.packets_sent++;

                            sock->state = RUDP_STATE_ESTABLISHED;
                            return 0;
                        }
                    }
                }
            }
        }
        retries++;
    }

    sock->state = RUDP_STATE_CLOSED;
    return -1;
}

int rudp_send(rudp_socket_t *sock, const void *buf, size_t len) {
    if (!sock || !buf || sock->state != RUDP_STATE_ESTABLISHED) {
        return -1;
    }

    const uint8_t *data = (const uint8_t*)buf;
    size_t bytes_queued = 0;

    while (bytes_queued < len || !rudp_send_window_is_empty(&sock->send_win)) {
        /* Push as many packets as window credits permit */
        while (bytes_queued < len && rudp_send_window_can_send(&sock->send_win)) {
            size_t chunk_len = len - bytes_queued;
            if (chunk_len > RUDP_MAX_PAYLOAD_SIZE) {
                chunk_len = RUDP_MAX_PAYLOAD_SIZE;
            }

            rudp_packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.header.seq_num = 0; /* Will be assigned by window push */
            pkt.header.ack_num = sock->recv_win.expected_seq;
            pkt.header.flags = RUDP_FLAG_DATA;
            pkt.header.window_size = RUDP_DEFAULT_WINDOW_SIZE;
            pkt.header.payload_len = (uint16_t)chunk_len;
            memcpy(pkt.payload, data + bytes_queued, chunk_len);

            uint32_t seq = rudp_send_window_push(&sock->send_win, &pkt);
            if (seq == 0) {
                break; /* Window full */
            }

            /* Send packet on wire */
            pkt.header.seq_num = seq;
            rudp_send_raw_packet(sock, &pkt);
            sock->stats.packets_sent++;
            sock->stats.bytes_sent += chunk_len;

            bytes_queued += chunk_len;
        }

        /* Drive network events: process ACKs and handle retransmission */
        int res = rudp_drive_io(sock, 10);
        if (res < 0) {
            return res; /* Connection dropped or link timeout */
        }
    }

    return (int)len;
}

int rudp_recv(rudp_socket_t *sock, void *buf, size_t max_len, int timeout_ms) {
    if (!sock || !buf || max_len == 0) return -1;

    uint64_t start_ms = rudp_get_time_ms();

    while (sock->rx_stream.count == 0) {
        if (sock->peer_fin_received && sock->recv_win.slots[sock->recv_win.expected_seq % RUDP_MAX_WINDOW_SIZE].received == false) {
            return 0; /* EOF: remote party completed graceful shutdown */
        }

        if (timeout_ms >= 0 && (rudp_get_time_ms() - start_ms) >= (uint64_t)timeout_ms) {
            return -1; /* Timed out waiting for data */
        }

        int res = rudp_drive_io(sock, 10);
        if (res < 0) {
            return res;
        }
    }

    return (int)rudp_fifo_read(&sock->rx_stream, (uint8_t*)buf, max_len);
}

int rudp_close(rudp_socket_t *sock) {
    if (!sock) return 0;

    if (sock->state == RUDP_STATE_ESTABLISHED && sock->has_remote) {
        /* Send FIN packet */
        rudp_packet_t fin_pkt;
        memset(&fin_pkt, 0, sizeof(fin_pkt));
        fin_pkt.header.seq_num = sock->send_win.next_seq;
        fin_pkt.header.ack_num = sock->recv_win.expected_seq;
        fin_pkt.header.flags = RUDP_FLAG_FIN;
        fin_pkt.header.window_size = RUDP_DEFAULT_WINDOW_SIZE;
        fin_pkt.header.payload_len = 0;

        for (int i = 0; i < 3; ++i) {
            rudp_send_raw_packet(sock, &fin_pkt);
            sock->stats.packets_sent++;
            sock->stats.fin_retries++;

            uint64_t fin_start = rudp_get_time_ms();
            while ((rudp_get_time_ms() - fin_start) < 150) {
                rudp_drive_io(sock, 20);
                if (sock->peer_fin_received) {
                    break;
                }
            }
            if (sock->peer_fin_received) break;
        }
    }

    sock->state = RUDP_STATE_CLOSED;
    rudp_close_socket(sock->raw_fd);
    free(sock);
    return 0;
}

void rudp_get_stats(const rudp_socket_t *sock, rudp_stats_t *stats) {
    if (sock && stats) {
        *stats = sock->stats;
    }
}
