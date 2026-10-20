#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "rudp_socket.h"
#include "rudp_crypto.h"

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -f <file>       Path to file to transmit (required)\n");
    printf("  -s <ip>         Target receiver/proxy IP (default: 127.0.0.1)\n");
    printf("  -p <port>       Target receiver/proxy port (default: 8000)\n");
    printf("  -h              Show help\n");
}

int main(int argc, char *argv[]) {
    const char *filepath = NULL;
    const char *target_ip = "127.0.0.1";
    uint16_t target_port = 8000;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            filepath = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            target_ip = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            target_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (!filepath) {
        fprintf(stderr, "[ERROR] Please specify an input file with -f <file>\n");
        print_usage(argv[0]);
        return 1;
    }

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        fprintf(stderr, "[ERROR] Unable to open input file %s\n", filepath);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize_signed = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize_signed < 0) {
        fprintf(stderr, "[ERROR] Invalid file size\n");
        fclose(fp);
        return 1;
    }
    uint64_t file_size = (uint64_t)fsize_signed;

    /* Pre-calculate SHA-256 for sender verification */
    char source_hash[65] = {0};
    rudp_sha256_file(filepath, source_hash);

    printf("====================================================================\n");
    printf("   Qualcomm Portfolio Demo: RUDP High-Performance File Sender       \n");
    printf("====================================================================\n");
    printf(" File to Send:     %s\n", filepath);
    printf(" Payload Size:     %" PRIu64 " bytes (%.2f MB)\n",
           file_size, (double)file_size / (1024.0 * 1024.0));
    printf(" Source SHA-256:   %s\n", source_hash);
    printf(" Target Endpoint:  %s:%u\n", target_ip, target_port);
    printf(" Initiating RUDP 3-Way Handshake (SYN -> SYN-ACK -> ACK)...\n\n");

    rudp_socket_t *sock = rudp_socket();
    if (!sock) {
        fprintf(stderr, "[ERROR] Failed to allocate RUDP socket\n");
        fclose(fp);
        return 1;
    }

    if (rudp_connect(sock, target_ip, target_port) != 0) {
        fprintf(stderr, "[ERROR] Handshake failed: could not connect to %s:%u\n",
                target_ip, target_port);
        rudp_close(sock);
        fclose(fp);
        return 1;
    }

    printf("[HANDSHAKE] 3-Way Handshake SUCCESS! Link State: ESTABLISHED\n");

    /* 1. Transmit 8-byte file length header */
    uint8_t size_header[8];
    for (int i = 7; i >= 0; --i) {
        size_header[i] = (uint8_t)(file_size & 0xFF);
        file_size >>= 8;
    }
    file_size = (uint64_t)fsize_signed; /* Restore original */

    if (rudp_send(sock, size_header, 8) < 0) {
        fprintf(stderr, "[ERROR] Failed to transmit file header\n");
        rudp_close(sock);
        fclose(fp);
        return 1;
    }

    /* 2. Stream file content over sliding window */
    uint8_t send_buf[65536];
    uint64_t bytes_sent = 0;
    uint64_t t_start = rudp_get_time_ms();
    uint64_t last_progress_time = t_start;

    while (bytes_sent < file_size) {
        size_t to_read = sizeof(send_buf);
        if ((file_size - bytes_sent) < to_read) {
            to_read = (size_t)(file_size - bytes_sent);
        }

        size_t n_read = fread(send_buf, 1, to_read, fp);
        if (n_read == 0) break;

        int sent = rudp_send(sock, send_buf, n_read);
        if (sent < 0) {
            fprintf(stderr, "[ERROR] Fatal transmission error over RUDP socket\n");
            break;
        }

        bytes_sent += (size_t)sent;

        uint64_t now = rudp_get_time_ms();
        if (now - last_progress_time >= 1000 || bytes_sent == file_size) {
            double pct = (100.0 * bytes_sent) / file_size;
            double elapsed = (now - t_start) / 1000.0;
            double speed = elapsed > 0 ? (bytes_sent / (1024.0 * 1024.0 * elapsed)) : 0.0;
            printf("[SENDER PROGRESS] %6.2f%% | %" PRIu64 "/%" PRIu64 " bytes | %.2f MB/s\n",
                   pct, bytes_sent, file_size, speed);
            last_progress_time = now;
        }
    }

    fclose(fp);
    uint64_t t_end = rudp_get_time_ms();
    double total_sec = (t_end - t_start) / 1000.0;
    if (total_sec <= 0.0) total_sec = 0.001;

    /* 3. Collect statistics */
    rudp_stats_t stats;
    rudp_get_stats(sock, &stats);

    printf("\n[TEARDOWN] Transmission complete. Gracefully terminating connection (FIN)...\n");
    rudp_close(sock);

    printf("\n====================================================================\n");
    printf("   RUDP Sender Protocol Metrics & Link Performance Summary           \n");
    printf("====================================================================\n");
    printf(" Total Stream Bytes:      %" PRIu64 " bytes (%.2f MB)\n",
           bytes_sent, (double)bytes_sent / (1024.0 * 1024.0));
    printf(" Elapsed Duration:        %.2f seconds\n", total_sec);
    printf(" Effective Goodput:       %.2f MB/s (%.2f Mbps)\n",
           (bytes_sent / (1024.0 * 1024.0)) / total_sec,
           (bytes_sent * 8.0 / (1000.0 * 1000.0)) / total_sec);
    printf(" Packets Transmitted:     %u\n", stats.packets_sent);
    printf(" Packets Retransmitted:   %u (Loss recovery rate: %.1f%%)\n",
           stats.packets_retransmitted,
           stats.packets_sent > 0 ? (100.0 * stats.packets_retransmitted / stats.packets_sent) : 0.0);
    printf(" Fast Retransmits:        %u (Sub-RTO 3-DupACK recovery)\n",
           stats.fast_retransmissions);
    printf(" Pre-Transfer SHA-256:    %s\n", source_hash);
    printf("====================================================================\n\n");

    rudp_platform_cleanup();
    return 0;
}
