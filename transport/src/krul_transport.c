#include "krul_transport.h"

#include <limits.h>
#include <string.h>

static const uint8_t json_magic[4] = {'K', 'R', 'J', '1'};
static const uint8_t bson_magic[4] = {'K', 'R', 'B', '1'};
static const uint8_t cbor_magic[4] = {'K', 'R', 'C', '1'};

enum {
    PARSER_SEARCH_MAGIC = 0,
    PARSER_READ_LENGTH,
    PARSER_READ_PAYLOAD,
    PARSER_READ_CRC_LOW,
    PARSER_READ_CRC_HIGH,
    PARSER_FRAME_READY
};

static uint16_t crc_update(uint16_t crc, uint8_t value) {
    crc ^= (uint16_t)value << 8U;
    for (unsigned int bit = 0U; bit < 8U; ++bit)
        crc = (uint16_t)((crc & 0x8000U) != 0U ? (crc << 1U) ^ 0x1021U
                                                   : crc << 1U);
    return crc;
}

uint16_t krul_transport_crc16(const uint8_t* data, size_t size) {
    if (data == NULL && size != 0U) return 0U;
    uint16_t crc = 0xFFFFU;
    for (size_t index = 0U; index < size; ++index)
        crc = crc_update(crc, data[index]);
    return crc;
}

const uint8_t* krul_transport_magic(krul_transport_format_t format) {
    if (format == KRUL_TRANSPORT_FORMAT_JSON) return json_magic;
    if (format == KRUL_TRANSPORT_FORMAT_BSON) return bson_magic;
    if (format == KRUL_TRANSPORT_FORMAT_CBOR) return cbor_magic;
    return NULL;
}

static uint32_t read_u32_le(const uint8_t* value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static void write_u32_le(uint8_t* output, uint32_t value) {
    for (uint32_t index = 0U; index < 4U; ++index)
        output[index] = (uint8_t)(value >> (index * 8U));
}

static void clear_frame(krul_transport_parser_t* parser, bool clear_window) {
    parser->payload_size = 0U;
    parser->payload_received = 0U;
    parser->expected_length = 0U;
    parser->calculated_crc = 0xFFFFU;
    parser->received_crc = 0U;
    parser->length_received = 0U;
    parser->format = KRUL_TRANSPORT_FORMAT_INVALID;
    parser->state = PARSER_SEARCH_MAGIC;
    if (clear_window) parser->magic_window = 0U;
}

bool krul_transport_parser_init(krul_transport_parser_t* parser,
                                uint8_t* payload, size_t payload_capacity) {
    if (parser == NULL || payload == NULL || payload_capacity == 0U ||
        payload_capacity > UINT32_MAX)
        return false;
    *parser = (krul_transport_parser_t){.payload = payload,
                                        .payload_capacity = payload_capacity,
                                        .calculated_crc = 0xFFFFU,
                                        .initialized = true};
    return true;
}

void krul_transport_parser_reset(krul_transport_parser_t* parser) {
    if (parser == NULL || !parser->initialized) return;
    clear_frame(parser, true);
}

uint32_t magic_to_uint32(const uint8_t *magic) {
	uint32_t num = 0;
	num = (magic[0] << 8*3) | (magic[1] << 8*2) | (magic[2] << 8*1) | (magic[3] << 8*0);
	return num;
}

static bool window_is_magic(krul_transport_parser_t* parser) {
    if (parser->magic_window == magic_to_uint32(json_magic)) {
        parser->format = KRUL_TRANSPORT_FORMAT_JSON;
    } else if (parser->magic_window == magic_to_uint32(bson_magic)) {
        parser->format = KRUL_TRANSPORT_FORMAT_BSON;
    } else if (parser->magic_window == magic_to_uint32(cbor_magic)) {
        parser->format = KRUL_TRANSPORT_FORMAT_CBOR;
    } else {
        return false;
    }
    parser->state = PARSER_READ_LENGTH;
    parser->length_received = 0U;
    parser->calculated_crc = 0xFFFFU;
    return true;
}

static void resync_after_error(krul_transport_parser_t* parser) {
    uint32_t window = parser->magic_window;
    clear_frame(parser, false);
    parser->magic_window = window;
    (void)window_is_magic(parser);
}

krul_transport_status_t krul_transport_parser_consume(
    krul_transport_parser_t* parser, const uint8_t* input, size_t input_size,
    size_t* consumed) {
    if (consumed != NULL) *consumed = 0U;
    if (parser == NULL || !parser->initialized ||
        (input == NULL && input_size != 0U) ||
        parser->state == PARSER_FRAME_READY)
        return KRUL_TRANSPORT_INVALID_STATE;

    for (size_t index = 0U; index < input_size; ++index) {
        uint8_t byte = input[index];
        parser->magic_window = (parser->magic_window << 8U) | byte;
        if (consumed != NULL) *consumed = index + 1U;
        switch (parser->state) {
            case PARSER_SEARCH_MAGIC:
                (void)window_is_magic(parser);
                break;
            case PARSER_READ_LENGTH:
                parser->length_bytes[parser->length_received++] = byte;
                parser->calculated_crc =
                    crc_update(parser->calculated_crc, byte);
                if (parser->length_received == 4U) {
                    parser->expected_length =
                        read_u32_le(parser->length_bytes);
                    if (parser->expected_length == 0U ||
                        parser->expected_length > parser->payload_capacity) {
                        resync_after_error(parser);
                        return KRUL_TRANSPORT_INVALID_LENGTH;
                    }
                    parser->state = PARSER_READ_PAYLOAD;
                }
                break;
            case PARSER_READ_PAYLOAD:
                parser->payload[parser->payload_received++] = byte;
                parser->calculated_crc =
                    crc_update(parser->calculated_crc, byte);
                if (parser->payload_received == parser->expected_length)
                    parser->state = PARSER_READ_CRC_LOW;
                break;
            case PARSER_READ_CRC_LOW:
                parser->received_crc = byte;
                parser->state = PARSER_READ_CRC_HIGH;
                break;
            case PARSER_READ_CRC_HIGH:
                parser->received_crc |= (uint16_t)byte << 8U;
                if (parser->received_crc != parser->calculated_crc) {
                    resync_after_error(parser);
                    return KRUL_TRANSPORT_BAD_CRC;
                }
                parser->payload_size = parser->payload_received;
                parser->state = PARSER_FRAME_READY;
                return KRUL_TRANSPORT_FRAME_READY;
            default: return KRUL_TRANSPORT_INVALID_STATE;
        }
    }
    return KRUL_TRANSPORT_NEED_MORE;
}

size_t krul_transport_encode(krul_transport_format_t format,
                             const uint8_t* payload, size_t payload_size,
                             uint8_t* output, size_t output_capacity) {
    const uint8_t* magic = krul_transport_magic(format);
    if (magic == NULL || payload == NULL || payload_size == 0U ||
        payload_size > UINT32_MAX || output == NULL ||
        output_capacity < KRUL_TRANSPORT_OVERHEAD ||
        payload_size > output_capacity - KRUL_TRANSPORT_OVERHEAD)
        return 0U;
    memcpy(output, magic, 4U);
    write_u32_le(output + 4U, (uint32_t)payload_size);
    memmove(output + KRUL_TRANSPORT_HEADER_SIZE, payload, payload_size);
    uint16_t crc = krul_transport_crc16(output + 4U, 4U + payload_size);
    output[KRUL_TRANSPORT_HEADER_SIZE + payload_size] = (uint8_t)crc;
    output[KRUL_TRANSPORT_HEADER_SIZE + payload_size + 1U] =
        (uint8_t)(crc >> 8U);
    return payload_size + KRUL_TRANSPORT_OVERHEAD;
}
