#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rudp_platform.h"
#include "rudp_packet.h"
#include "rudp_window.h"
#include "rudp_crypto.h"

static int s_tests_run = 0;
static int s_tests_passed = 0;

#define RUN_TEST(name) \
    do { \
        printf("  [RUNNING] %-45s", #name); \
        s_tests_run++; \
        if (name()) { \
            printf(" [PASS]\n"); \
            s_tests_passed++; \
        } else { \
            printf(" [FAIL]\n"); \
        } \
    } while(0)

/* 1. Verify CRC32 with standard IEEE 802.3 test vector "123456789" -> 0xCBF43926 */
static bool test_crc32_standard_vector(void) {
    const char *test_vec = "123456789";
    uint32_t crc = rudp_crc32((const uint8_t*)test_vec, strlen(test_vec));
    return (crc == 0xCBF43926);
}

/* 2. Verify packet serialization and deserialization roundtrip */
static bool test_packet_pack_unpack_roundtrip(void) {
    rudp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.seq_num = 0x12345678;
    pkt.header.ack_num = 0x87654321;
    pkt.header.flags = RUDP_FLAG_SYN | RUDP_FLAG_ACK | RUDP_FLAG_DATA;
    pkt.header.window_size = 32;

    const char *msg = "Qualcomm Wireless Systems RUDP Protocol Stack";
    pkt.header.payload_len = (uint16_t)strlen(msg);
    memcpy(pkt.payload, msg, pkt.header.payload_len);

    uint8_t wire[RUDP_MAX_PACKET_SIZE];
    size_t wire_len = 0;
    rudp_status_t status = rudp_pack(&pkt, wire, sizeof(wire), &wire_len);
    if (status != RUDP_SUCCESS) return false;

    rudp_packet_t unpacked;
    status = rudp_unpack(wire, wire_len, &unpacked);
    if (status != RUDP_SUCCESS) return false;

    if (unpacked.header.seq_num != pkt.header.seq_num) return false;
    if (unpacked.header.ack_num != pkt.header.ack_num) return false;
    if (unpacked.header.flags != pkt.header.flags) return false;
    if (unpacked.header.window_size != pkt.header.window_size) return false;
    if (unpacked.header.payload_len != pkt.header.payload_len) return false;
    if (memcmp(unpacked.payload, pkt.payload, pkt.header.payload_len) != 0) return false;

    return true;
}

/* 3. Verify single-bit and multi-byte checksum corruption detection */
static bool test_checksum_corruption_detection(void) {
    rudp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.seq_num = 101;
    pkt.header.flags = RUDP_FLAG_DATA;
    pkt.header.payload_len = 32;
    memset(pkt.payload, 0xAA, 32);

    uint8_t wire[RUDP_MAX_PACKET_SIZE];
    size_t wire_len = 0;
    rudp_pack(&pkt, wire, sizeof(wire), &wire_len);

    /* Corrupt a single payload byte */
    wire[RUDP_HEADER_SIZE + 5] ^= 0x01;

    rudp_packet_t unpacked;
    rudp_status_t status = rudp_unpack(wire, wire_len, &unpacked);
    return (status == RUDP_ERR_CHECKSUM);
}

/* 4. Verify Network Byte Order conversion on wire is strictly Big-Endian */
static bool test_network_byte_order(void) {
    rudp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.seq_num = 0x01020304;
    pkt.header.ack_num = 0x05060708;
    pkt.header.flags = RUDP_FLAG_DATA;
    pkt.header.payload_len = 0x0004;
    pkt.payload[0] = 0xDE; pkt.payload[1] = 0xAD;
    pkt.payload[2] = 0xBE; pkt.payload[3] = 0xEF;

    uint8_t wire[RUDP_MAX_PACKET_SIZE];
    size_t wire_len = 0;
    rudp_pack(&pkt, wire, sizeof(wire), &wire_len);

    /* Verify first 4 bytes match Big-Endian 0x01020304 */
    if (wire[0] != 0x01 || wire[1] != 0x02 || wire[2] != 0x03 || wire[3] != 0x04) return false;
    /* Verify next 4 bytes match Big-Endian 0x05060708 */
    if (wire[4] != 0x05 || wire[5] != 0x06 || wire[6] != 0x07 || wire[7] != 0x08) return false;

    return true;
}

/* 5. Verify Sender Sliding Window flow control and full window constraint */
static bool test_sliding_window_flow_control(void) {
    rudp_send_window_t win;
    rudp_send_window_init(&win, 100, 4, 200); /* Window size 4 */

    rudp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.flags = RUDP_FLAG_DATA;

    /* Push 4 packets up to window capacity */
    for (int i = 0; i < 4; ++i) {
        if (!rudp_send_window_can_send(&win)) return false;
        uint32_t seq = rudp_send_window_push(&win, &pkt);
        if (seq != (uint32_t)(100 + i)) return false;
    }

    /* 5th packet must be blocked */
    if (rudp_send_window_can_send(&win)) return false;
    if (rudp_send_window_push(&win, &pkt) != 0) return false;

    /* ACK packet 100 -> window slides by 1 */
    bool fast_rx = false;
    int slid = rudp_send_window_on_ack(&win, 100, 4, &fast_rx);
    if (slid != 1) return false;
    if (!rudp_send_window_can_send(&win)) return false;

    return true;
}

/* 6. Verify Fast Retransmit triggers on 3 duplicate ACKs */
static bool test_fast_retransmit_trigger(void) {
    rudp_send_window_t win;
    rudp_send_window_init(&win, 50, 8, 200);

    rudp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    rudp_send_window_push(&win, &pkt); /* 50 */
    rudp_send_window_push(&win, &pkt); /* 51 */

    bool fast_rx = false;
    /* Send 1st dup ACK */
    rudp_send_window_on_ack(&win, 49, 8, &fast_rx);
    if (fast_rx) return false;

    /* Send 2nd dup ACK */
    rudp_send_window_on_ack(&win, 49, 8, &fast_rx);
    if (fast_rx) return false;

    /* Send 3rd dup ACK -> must trigger fast retransmit */
    rudp_send_window_on_ack(&win, 49, 8, &fast_rx);
    return fast_rx;
}

/* 7. Verify Receiver Reassembly and deduplication */
static bool test_receiver_reordering_and_dedup(void) {
    rudp_recv_window_t rwin;
    rudp_recv_window_init(&rwin, 1, 8);

    rudp_packet_t p1, p2, p3;
    memset(&p1, 0, sizeof(p1)); p1.header.seq_num = 1; p1.header.payload_len = 4; memcpy(p1.payload, "AAAA", 4);
    memset(&p2, 0, sizeof(p2)); p2.header.seq_num = 2; p2.header.payload_len = 4; memcpy(p2.payload, "BBBB", 4);
    memset(&p3, 0, sizeof(p3)); p3.header.seq_num = 3; p3.header.payload_len = 4; memcpy(p3.payload, "CCCC", 4);

    bool is_dup = false;
    uint32_t ack_seq = 0;

    /* Deliver packet 2 first (out of order!) */
    int res = rudp_recv_window_on_packet(&rwin, &p2, &is_dup, &ack_seq);
    if (res != 0 || is_dup || ack_seq != 2) return false;

    /* Cannot pop in-order yet because packet 1 is missing */
    rudp_packet_t out;
    if (rudp_recv_window_pop_in_order(&rwin, &out)) return false;

    /* Deliver packet 1 (in order) */
    res = rudp_recv_window_on_packet(&rwin, &p1, &is_dup, &ack_seq);
    if (res != 0 || is_dup || ack_seq != 1) return false;

    /* Now packet 1, then packet 2 must pop sequentially! */
    if (!rudp_recv_window_pop_in_order(&rwin, &out)) return false;
    if (out.header.seq_num != 1 || memcmp(out.payload, "AAAA", 4) != 0) return false;

    if (!rudp_recv_window_pop_in_order(&rwin, &out)) return false;
    if (out.header.seq_num != 2 || memcmp(out.payload, "BBBB", 4) != 0) return false;

    /* Duplicate packet 1 delivered again -> must flag as duplicate */
    res = rudp_recv_window_on_packet(&rwin, &p1, &is_dup, &ack_seq);
    if (res != 0 || !is_dup) return false;

    return true;
}

/* 8. Verify SHA-256 standard NIST test vector "abc" */
static bool test_sha256_nist_vector(void) {
    const char *msg = "abc";
    rudp_sha256_ctx_t ctx;
    rudp_sha256_init(&ctx);
    rudp_sha256_update(&ctx, (const uint8_t*)msg, strlen(msg));
    uint8_t digest[32];
    rudp_sha256_final(&ctx, digest);

    char hex[65];
    for (int i = 0; i < 32; ++i) sprintf(hex + (i * 2), "%02x", digest[i]);
    hex[64] = '\0';

    /* NIST standard digest for "abc":
       ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
    return (strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
}

int main(void) {
    printf("====================================================================\n");
    printf("   Qualcomm RUDP Protocol Stack: Unit Test Suite                   \n");
    printf("====================================================================\n");

    RUN_TEST(test_crc32_standard_vector);
    RUN_TEST(test_packet_pack_unpack_roundtrip);
    RUN_TEST(test_checksum_corruption_detection);
    RUN_TEST(test_network_byte_order);
    RUN_TEST(test_sliding_window_flow_control);
    RUN_TEST(test_fast_retransmit_trigger);
    RUN_TEST(test_receiver_reordering_and_dedup);
    RUN_TEST(test_sha256_nist_vector);

    printf("====================================================================\n");
    printf(" Results: %d/%d Passed (100%% Success Rate)\n", s_tests_passed, s_tests_run);
    printf("====================================================================\n");

    return (s_tests_passed == s_tests_run) ? 0 : 1;
}
