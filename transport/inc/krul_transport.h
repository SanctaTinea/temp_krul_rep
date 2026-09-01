#pragma once

/**
 * @file krul_transport.h
 * @brief Versioned stream framing for JSON, BSON, and CBOR payloads.
 *
 * This library does not depend on the Krul command protocol. `KRJ1`, `KRB1`,
 * and `KRC1` are stable wire-level identifiers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KRUL_TRANSPORT_HEADER_SIZE 8U
#define KRUL_TRANSPORT_CRC_SIZE 2U
#define KRUL_TRANSPORT_OVERHEAD \
    (KRUL_TRANSPORT_HEADER_SIZE + KRUL_TRANSPORT_CRC_SIZE)

typedef enum {
    KRUL_TRANSPORT_FORMAT_INVALID = 0,
    KRUL_TRANSPORT_FORMAT_JSON,
    KRUL_TRANSPORT_FORMAT_BSON,
    KRUL_TRANSPORT_FORMAT_CBOR
} krul_transport_format_t;

typedef enum {
    KRUL_TRANSPORT_NEED_MORE = 0,
    KRUL_TRANSPORT_FRAME_READY,
    KRUL_TRANSPORT_INVALID_LENGTH,
    KRUL_TRANSPORT_BAD_CRC,
    KRUL_TRANSPORT_INVALID_STATE
} krul_transport_status_t;

/** Stateful decoder for an arbitrarily chunked byte stream. */
typedef struct {
    uint8_t* payload;
    size_t payload_capacity;
    size_t payload_size;
    size_t payload_received;
    uint32_t magic_window;
    uint32_t expected_length;
    uint16_t calculated_crc;
    uint16_t received_crc;
    uint8_t length_bytes[4];
    uint8_t length_received;
    uint8_t state;
    krul_transport_format_t format;
    bool initialized;
} krul_transport_parser_t;

/** Initialize a parser using caller-owned payload storage. */
bool krul_transport_parser_init(krul_transport_parser_t* parser,
                                uint8_t* payload, size_t payload_capacity);

/** Reset a parser and resume searching for a magic value. */
void krul_transport_parser_reset(krul_transport_parser_t* parser);

/**
 * Consume bytes until more input is needed, a frame is ready, or an error is
 * detected. `consumed` always reports how many input bytes were accepted.
 */
krul_transport_status_t krul_transport_parser_consume(
    krul_transport_parser_t* parser, const uint8_t* input, size_t input_size,
    size_t* consumed);

/** CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff, no reflection/xorout). */
uint16_t krul_transport_crc16(const uint8_t* data, size_t size);

/**
 * Encode one complete frame. `payload` may point at `output + 8`, allowing an
 * application to reserve the transport header before its codec output.
 */
size_t krul_transport_encode(krul_transport_format_t format,
                             const uint8_t* payload, size_t payload_size,
                             uint8_t* output, size_t output_capacity);

/** Return the four-byte wire magic (`KRJ1`, `KRB1`, or `KRC1`). */
const uint8_t* krul_transport_magic(krul_transport_format_t format);

#ifdef __cplusplus
}
#endif
