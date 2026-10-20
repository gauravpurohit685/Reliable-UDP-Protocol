#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "rudp_socket.h"
#include "rudp_crypto.h"

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p <port>        Listen port (default: 9001)\n");
    printf("  -o <output_file> Output filename (default: received_file.dat)\n");
    printf("  -h               Show help\n");
}

int main(int argc, char *argv[]) {
    uint16_t listen_port = 9001;
    const char *output_file = "received_file.dat";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            listen_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("====================================================================\n");
    printf("   Qualcomm Portfolio Demo: RUDP File Receiver                      \n");
    printf("====================================================================\n");
    printf(" Binding Port: %u\n", listen_port);
    printf(" Output Target: %s\n", output_file);
    printf(" Waiting for incoming RUDP 3-Way Handshake (SYN)...\n\n");

    rudp_socket_t *server = rudp_socket();
    if (!server) {
        fprintf(stderr, "[ERROR] Failed to allocate RUDP server socket\n");
        return 1;
    }

    if (rudp_bind(server, "0.0.0.0", listen_port) != 0) {
        fprintf(stderr, "[ERROR] Failed to bind to port %u\n", listen_port);
        rudp_close(server);
        return 1;
    }

    rudp_listen(server);

    rudp_socket_t *client = rudp_accept(server, 30000);
    if (!client) {
        fprintf(stderr, "[ERROR] Handshake accept timed out\n");
        rudp_close(server);
        return 1;
    }

    printf("[HANDSHAKE] Connection ESTABLISHED with %s:%u\n",
           inet_ntoa(client->remote_addr.sin_addr),
           ntohs(client->remote_addr.sin_port));

    /* 1. Receive 8-byte file length header */
    uint64_t expected_filesize = 0;
    uint8_t size_header[8];
    size_t header_bytes = 0;

    while (header_bytes < 8) {
        int r = rudp_recv(client, size_header + header_bytes, 8 - header_bytes, 10000);
        if (r <= 0) {
            fprintf(stderr, "[ERROR] Failed to receive file size header\n");
            rudp_close(client);
            rudp_close(server);
            return 1;
        }
        header_bytes += (size_t)r;
    }

    for (int i = 0; i < 8; ++i) {
        expected_filesize = (expected_filesize << 8) | size_header[i];
    }
    printf("[TRANSFER] Incoming file payload: %" PRIu64 " bytes (%.2f MB)\n",
           expected_filesize, (double)expected_filesize / (1024.0 * 1024.0));

    FILE *out_fp = fopen(output_file, "wb");
    if (!out_fp) {
        fprintf(stderr, "[ERROR] Cannot open output file %s\n", output_file);
        rudp_close(client);
        rudp_close(server);
        return 1;
    }

    /* 2. Stream reception until expected size reached */
    uint8_t recv_buf[65536];
    uint64_t total_received = 0;
    uint64_t t_start = rudp_get_time_ms();
    uint64_t last_progress_time = t_start;

    while (total_received < expected_filesize) {
        size_t to_read = sizeof(recv_buf);
        if ((expected_filesize - total_received) < to_read) {
            to_read = (size_t)(expected_filesize - total_received);
        }

        int r = rudp_recv(client, recv_buf, to_read, 10000);
        if (r < 0) {
            fprintf(stderr, "[ERROR] Receive timeout or connection drop\n");
            break;
        }
        if (r == 0) {
            /* EOF received */
            break;
        }

        fwrite(recv_buf, 1, (size_t)r, out_fp);
        total_received += (size_t)r;

        uint64_t now = rudp_get_time_ms();
        if (now - last_progress_time >= 1000 || total_received == expected_filesize) {
            double pct = (100.0 * total_received) / expected_filesize;
            double elapsed = (now - t_start) / 1000.0;
            double speed = elapsed > 0 ? (total_received / (1024.0 * 1024.0 * elapsed)) : 0.0;
            printf("[PROGRESS] %6.2f%% | %" PRIu64 "/%" PRIu64 " bytes | %.2f MB/s\n",
                   pct, total_received, expected_filesize, speed);
            last_progress_time = now;
        }
    }

    fclose(out_fp);
    uint64_t t_end = rudp_get_time_ms();
    double total_sec = (t_end - t_start) / 1000.0;
    if (total_sec <= 0.0) total_sec = 0.001;

    /* 3. Compute SHA-256 of reconstructed file */
    char file_hash[65] = {0};
    if (rudp_sha256_file(output_file, file_hash) == 0) {
        printf("\n[INTEGRITY] Reconstructed File SHA-256:\n  -> %s\n", file_hash);
    }

    /* 4. Telemetry and protocol metrics */
    rudp_stats_t stats;
    rudp_get_stats(client, &stats);

    printf("\n====================================================================\n");
    printf("   RUDP Receiver Protocol Metrics & Link Performance Summary         \n");
    printf("====================================================================\n");
    printf(" Total Received Bytes:    %" PRIu64 " bytes (%.2f MB)\n",
           total_received, (double)total_received / (1024.0 * 1024.0));
    printf(" Elapsed Transfer Time:   %.2f seconds\n", total_sec);
    printf(" Effective Goodput:       %.2f MB/s (%.2f Mbps)\n",
           (total_received / (1024.0 * 1024.0)) / total_sec,
           (total_received * 8.0 / (1000.0 * 1000.0)) / total_sec);
    printf(" Packets Received:        %u\n", stats.packets_received);
    printf(" Duplicate Packets Purged:%u (Deduplicated successfully)\n",
           stats.duplicate_packets_received);
    printf(" Checksum Errors Caught:  %u (Corruptions rejected)\n",
           stats.checksum_errors);
    printf("====================================================================\n\n");

    rudp_close(client);
    rudp_close(server);
    rudp_platform_cleanup();
    return 0;
}
