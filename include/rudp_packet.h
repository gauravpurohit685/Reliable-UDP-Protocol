#ifndef RUDP_PACKET_H
#define RUDP_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "rudp_platform.h"

#define RUDP_PROTOCOL_VERSION 1

/* Protocol Control Flags */
#define RUDP_FLAG_SYN  (1U << 0) /* 0x01: Synchronize connection / 3-way handshake */
#define RUDP_FLAG_ACK  (1U << 1) /* 0x02: Acknowledgment number is valid */
#define RUDP_FLAG_FIN  (1U << 2) /* 0x04: Connection termination */
#define RUDP_FLAG_DATA (1U << 3) /* 0x08: Payload data transfer */
#define RUDP_FLAG_RST  (1U << 4) /* 0x10: Abrupt connection reset */

/* Size Constraints */
#define RUDP_HEADER_SIZE      16U
#define RUDP_MAX_PAYLOAD_SIZE 1400U
#define RUDP_MAX_PACKET_SIZE  (RUDP_HEADER_SIZE + RUDP_MAX_PAYLOAD_SIZE)

/* Status / Return Codes */
typedef enum {
    RUDP_SUCCESS              =  0,
    RUDP_ERR_GENERIC          = -1,
    RUDP_ERR_INVALID_PARAM    = -2,
    RUDP_ERR_BUFFER_TOO_SMALL = -3,
    RUDP_ERR_CHECKSUM         = -4,
    RUDP_ERR_MALFORMED        = -5,
    RUDP_ERR_TIMEOUT          = -6,
    RUDP_ERR_CONNECTION       = -7,
    RUDP_ERR_RESET            = -8,
    RUDP_ERR_CLOSED           = -9
} rudp_status_t;

/* Binary Packed RUDP Header (16 bytes, big-endian on wire) */
#pragma pack(push, 1)
typedef struct {
    uint32_t seq_num;     /* 4 bytes: Sequence Number */
    uint32_t ack_num;     /* 4 bytes: Acknowledgment Number */
    uint8_t  flags;       /* 1 byte:  SYN, ACK, FIN, DATA, RST */
    uint8_t  window_size; /* 1 byte:  Advertised receive window credit */
    uint16_t payload_len; /* 2 bytes: Length of payload in bytes */
    uint32_t checksum;    /* 4 bytes: IEEE 802.3 CRC32 checksum */
} __attribute__((packed)) rudp_header_t;
#pragma pack(pop)

/* In-memory RUDP Packet Structure (host byte order) */
typedef struct {
    rudp_header_t header;
    uint8_t payload[RUDP_MAX_PAYLOAD_SIZE];
} rudp_packet_t;

/**
 * @brief Computes IEEE 802.3 32-bit CRC.
 * @param data Pointer to input data buffer.
 * @param length Length of data in bytes.
 * @return 32-bit CRC value.
 */
uint32_t rudp_crc32(const uint8_t *data, size_t length);

/**
 * @brief Serializes an in-memory RUDP packet into a wire-format byte array.
 * Performs host-to-network byte order conversion and CRC32 calculation.
 *
 * @param pkt Pointer to input packet in host byte order.
 * @param wire_buf Output buffer to hold wire-formatted binary data.
 * @param max_wire_len Capacity of wire_buf.
 * @param[out] out_wire_len Actual number of bytes packed.
 * @return RUDP_SUCCESS on success, error code otherwise.
 */
rudp_status_t rudp_pack(const rudp_packet_t *pkt,
                        uint8_t *wire_buf,
                        size_t max_wire_len,
                        size_t *out_wire_len);

/**
 * @brief Deserializes a wire-format byte array into an in-memory RUDP packet.
 * Verifies CRC32 payload and header integrity, performs network-to-host byte order conversion.
 *
 * @param wire_buf Pointer to received wire-format bytes.
 * @param wire_len Number of bytes received.
 * @param[out] pkt Output packet structure in host byte order.
 * @return RUDP_SUCCESS on success, RUDP_ERR_CHECKSUM on corruption, or error code.
 */
rudp_status_t rudp_unpack(const uint8_t *wire_buf,
                          size_t wire_len,
                          rudp_packet_t *pkt);

/**
 * @brief Formats packet flags into a human-readable string (e.g. "[SYN|ACK]").
 * @param flags Bitmask of packet flags.
 * @param buf Output string buffer.
 * @param buf_len Size of output buffer.
 */
void rudp_flags_to_string(uint8_t flags, char *buf, size_t buf_len);

#endif /* RUDP_PACKET_H */
