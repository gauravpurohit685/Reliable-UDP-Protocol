#ifndef RUDP_CRYPTO_H
#define RUDP_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buffer[64];
} rudp_sha256_ctx_t;

/**
 * @brief Initializes SHA-256 context.
 */
void rudp_sha256_init(rudp_sha256_ctx_t *ctx);

/**
 * @brief Updates SHA-256 context with input chunk.
 */
void rudp_sha256_update(rudp_sha256_ctx_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Finalizes SHA-256 calculation and generates 32-byte digest.
 */
void rudp_sha256_final(rudp_sha256_ctx_t *ctx, uint8_t digest[32]);

/**
 * @brief Computes SHA-256 hash of a file on disk and outputs hex string.
 * @param filepath Path to file.
 * @param[out] output_hex Buffer of at least 65 bytes for null-terminated hex string.
 * @return 0 on success, negative on error.
 */
int rudp_sha256_file(const char *filepath, char output_hex[65]);

#endif /* RUDP_CRYPTO_H */
