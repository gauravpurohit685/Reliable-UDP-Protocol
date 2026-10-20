#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "rudp_platform.h"
#include "rudp_packet.h"

#define MAX_QUEUE_SIZE 1024

typedef struct {
    uint8_t            data[RUDP_MAX_PACKET_SIZE];
    size_t             len;
    struct sockaddr_in dest_addr;
    uint64_t           deliver_at_ms;
    bool               is_valid;
} delayed_packet_t;

typedef struct {
    uint32_t total_intercepted;
    uint32_t total_dropped;
    uint32_t total_duplicated;
    uint32_t total_delayed;
    uint32_t total_forwarded;
} proxy_stats_t;

static delayed_packet_t s_queue[MAX_QUEUE_SIZE];
static proxy_stats_t    s_stats = {0};

static uint32_t get_random_delay_ms(uint32_t min_ms, uint32_t max_ms) {
    if (max_ms <= min_ms) return min_ms;
    return min_ms + (rand() % (max_ms - min_ms + 1));
}

static bool queue_packet(const uint8_t *data, size_t len,
                         const struct sockaddr_in *dest, uint64_t deliver_at_ms) {
    for (int i = 0; i < MAX_QUEUE_SIZE; ++i) {
        if (!s_queue[i].is_valid) {
            memcpy(s_queue[i].data, data, len);
            s_queue[i].len = len;
            s_queue[i].dest_addr = *dest;
            s_queue[i].deliver_at_ms = deliver_at_ms;
            s_queue[i].is_valid = true;
            return true;
        }
    }
    return false; /* Queue overflow */
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -l <port>    Listen port (default: 8000)\n");
    printf("  -t <ip>      Target IP (default: 127.0.0.1)\n");
    printf("  -p <port>    Target port (default: 9001)\n");
    printf("  -d <rate>    Drop rate [0.0 - 1.0] (default: 0.15 for 15%% drop)\n");
    printf("  -u <rate>    Duplicate rate [0.0 - 1.0] (default: 0.05 for 5%% duplicate)\n");
    printf("  -m <ms>      Min delay jitter ms (default: 20)\n");
    printf("  -M <ms>      Max delay jitter ms (default: 50)\n");
    printf("  -v           Verbose packet logging\n");
    printf("  -h           Help\n");
}

int main(int argc, char *argv[]) {
    uint16_t listen_port = 8000;
    const char *target_ip = "127.0.0.1";
    uint16_t target_port = 9001;
    double drop_rate = 0.15;
    double dup_rate  = 0.05;
    uint32_t min_delay_ms = 20;
    uint32_t max_delay_ms = 50;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            listen_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            target_ip = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            target_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            drop_rate = atof(argv[++i]);
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            dup_rate = atof(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            min_delay_ms = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
            max_delay_ms = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    srand((unsigned int)time(NULL));
    rudp_platform_init();

    socket_t proxy_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy_sock == RUDP_INVALID_SOCKET) {
        fprintf(stderr, "[PROXY] Error creating socket\n");
        return 1;
    }

    if (rudp_set_nonblocking(proxy_sock, true) != 0) {
        fprintf(stderr, "[PROXY] Error setting nonblocking socket\n");
        rudp_close_socket(proxy_sock);
        return 1;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(listen_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(proxy_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        fprintf(stderr, "[PROXY] Bind failed on port %u\n", listen_port);
        rudp_close_socket(proxy_sock);
        return 1;
    }

    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(target_port);
    target_addr.sin_addr.s_addr = inet_addr(target_ip);

    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    bool has_client = false;

    printf("====================================================================\n");
    printf("   RUDP Network Impairment Simulator / Loss Proxy Active            \n");
    printf("====================================================================\n");
    printf(" Listening on Port:    %u\n", listen_port);
    printf(" Forward Target:       %s:%u\n", target_ip, target_port);
    printf(" Artificial Drop Rate: %.1f%%\n", drop_rate * 100.0);
    printf(" Duplication Rate:     %.1f%%\n", dup_rate * 100.0);
    printf(" Jitter Delay Range:   %u ms - %u ms\n", min_delay_ms, max_delay_ms);
    printf("====================================================================\n\n");

    uint64_t last_stats_print = rudp_get_time_ms();

    while (1) {
        uint64_t now = rudp_get_time_ms();

        /* 1. Check for incoming packets on listening socket */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(proxy_sock, &read_fds);

        struct timeval tv = {0, 1000}; /* 1 ms poll */
        int sel = select((int)proxy_sock + 1, &read_fds, NULL, NULL, &tv);

        if (sel > 0 && FD_ISSET(proxy_sock, &read_fds)) {
            uint8_t buffer[RUDP_MAX_PACKET_SIZE];
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);

            int recvd = recvfrom(proxy_sock, (char*)buffer, (int)sizeof(buffer), 0,
                                 (struct sockaddr*)&from_addr, &from_len);
            if (recvd > 0) {
                s_stats.total_intercepted++;

                /* Determine route: is packet from client or server? */
                bool is_from_server = (from_addr.sin_addr.s_addr == target_addr.sin_addr.s_addr &&
                                       from_addr.sin_port == target_addr.sin_port);

                struct sockaddr_in dest;
                if (is_from_server) {
                    if (!has_client) {
                        /* Client address unknown yet; drop */
                        continue;
                    }
                    dest = client_addr;
                } else {
                    /* Packet originates from client */
                    client_addr = from_addr;
                    has_client = true;
                    dest = target_addr;
                }

                /* Impairment 1: Random Packet Drop */
                double r_drop = (double)rand() / (double)RAND_MAX;
                if (r_drop < drop_rate) {
                    s_stats.total_dropped++;
                    if (verbose) {
                        printf("[SIM-DROP] Packet (%d bytes) intentionally DROPPED (drop roll: %.3f < %.3f)\n",
                               recvd, r_drop, drop_rate);
                    }
                    goto flush_queue;
                }

                /* Impairment 2: Jitter / Delay */
                uint32_t delay = get_random_delay_ms(min_delay_ms, max_delay_ms);
                uint64_t deliver_time = now + delay;
                queue_packet(buffer, (size_t)recvd, &dest, deliver_time);
                s_stats.total_delayed++;

                /* Impairment 3: Packet Duplication */
                double r_dup = (double)rand() / (double)RAND_MAX;
                if (r_dup < dup_rate) {
                    s_stats.total_duplicated++;
                    /* Queue duplicate with slight additional delay (5ms) */
                    queue_packet(buffer, (size_t)recvd, &dest, deliver_time + 5);
                    if (verbose) {
                        printf("[SIM-DUP] Packet DUPLICATED to %s:%u\n",
                               inet_ntoa(dest.sin_addr), ntohs(dest.sin_port));
                    }
                }
            }
        }

flush_queue:
        /* 2. Dispatch scheduled delayed packets */
        now = rudp_get_time_ms();
        for (int i = 0; i < MAX_QUEUE_SIZE; ++i) {
            if (s_queue[i].is_valid && now >= s_queue[i].deliver_at_ms) {
                sendto(proxy_sock,
                       (const char*)s_queue[i].data,
                       (int)s_queue[i].len,
                       0,
                       (struct sockaddr*)&s_queue[i].dest_addr,
                       sizeof(s_queue[i].dest_addr));
                s_stats.total_forwarded++;
                s_queue[i].is_valid = false;
            }
        }

        /* 3. Periodic telemetry report */
        if (now - last_stats_print >= 5000 && s_stats.total_intercepted > 0) {
            double actual_drop = (s_stats.total_intercepted > 0)
                               ? (100.0 * s_stats.total_dropped / s_stats.total_intercepted) : 0.0;
            printf("[PROXY STATS] Total: %-5u | Dropped: %-4u (%4.1f%%) | Duplicated: %-3u | Forwarded: %-5u\n",
                   s_stats.total_intercepted, s_stats.total_dropped, actual_drop,
                   s_stats.total_duplicated, s_stats.total_forwarded);
            last_stats_print = now;
        }
    }

    rudp_close_socket(proxy_sock);
    rudp_platform_cleanup();
    return 0;
}
