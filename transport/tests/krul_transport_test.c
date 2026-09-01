#include "krul_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const uint8_t check[] = "123456789";
    assert(krul_transport_crc16(check, sizeof(check) - 1U) == 0x29B1U);

    uint8_t payload_storage[64];
    uint8_t frame[128];
    static const uint8_t json[] = "{\"id\":1}";
    size_t frame_size = krul_transport_encode(
        KRUL_TRANSPORT_FORMAT_JSON, json, sizeof(json) - 1U, frame,
        sizeof(frame));
    assert(frame_size == sizeof(json) - 1U + KRUL_TRANSPORT_OVERHEAD);
    assert(memcmp(frame, "KRJ1", 4U) == 0);
    assert(frame[4] == sizeof(json) - 1U && frame[5] == 0U);

    krul_transport_parser_t parser;
    assert(krul_transport_parser_init(&parser, payload_storage,
                                      sizeof(payload_storage)));
    for (size_t index = 0U; index < frame_size; ++index) {
        size_t consumed = 0U;
        krul_transport_status_t status = krul_transport_parser_consume(
            &parser, &frame[index], 1U, &consumed);
        assert(consumed == 1U);
        assert(status == (index + 1U == frame_size
                              ? KRUL_TRANSPORT_FRAME_READY
                              : KRUL_TRANSPORT_NEED_MORE));
    }
    assert(parser.format == KRUL_TRANSPORT_FORMAT_JSON);
    assert(parser.payload_size == sizeof(json) - 1U);
    assert(memcmp(parser.payload, json, parser.payload_size) == 0);

    krul_transport_parser_reset(&parser);
    static const uint8_t bson[] = {5U, 0U, 0U, 0U, 0U};
    frame_size = krul_transport_encode(
        KRUL_TRANSPORT_FORMAT_BSON, bson, sizeof(bson), frame, sizeof(frame));
    assert(memcmp(frame, "KRB1", 4U) == 0);
    size_t consumed = 0U;
    assert(krul_transport_parser_consume(&parser, frame, frame_size,
                                         &consumed) ==
           KRUL_TRANSPORT_FRAME_READY);
    assert(consumed == frame_size);
    assert(parser.format == KRUL_TRANSPORT_FORMAT_BSON);

    krul_transport_parser_reset(&parser);
    static const uint8_t cbor[] = {0xbfU, 0x01U, 0x02U, 0xffU};
    frame_size = krul_transport_encode(
        KRUL_TRANSPORT_FORMAT_CBOR, cbor, sizeof(cbor), frame, sizeof(frame));
    assert(frame_size == sizeof(cbor) + KRUL_TRANSPORT_OVERHEAD);
    assert(memcmp(frame, "KRC1", 4U) == 0);
    consumed = 0U;
    assert(krul_transport_parser_consume(&parser, frame, frame_size,
                                         &consumed) ==
           KRUL_TRANSPORT_FRAME_READY);
    assert(parser.format == KRUL_TRANSPORT_FORMAT_CBOR);
    assert(parser.payload_size == sizeof(cbor));
    assert(memcmp(parser.payload, cbor, sizeof(cbor)) == 0);

    krul_transport_parser_reset(&parser);
    frame[frame_size - 1U] ^= 0x80U;
    assert(krul_transport_parser_consume(&parser, frame, frame_size,
                                         &consumed) ==
           KRUL_TRANSPORT_BAD_CRC);
    frame[frame_size - 1U] ^= 0x80U;
    assert(krul_transport_parser_consume(&parser, frame, frame_size,
                                         &consumed) ==
           KRUL_TRANSPORT_FRAME_READY);

    krul_transport_parser_reset(&parser);
    static const uint8_t bad_then_good_prefix[] = {
        'x', 'K', 'R', 'J', '1', 0xff, 0xff, 0xff, 0x7f};
    assert(krul_transport_parser_consume(
               &parser, bad_then_good_prefix, sizeof(bad_then_good_prefix),
               &consumed) == KRUL_TRANSPORT_INVALID_LENGTH);
    assert(krul_transport_parser_consume(&parser, frame, frame_size,
                                         &consumed) ==
           KRUL_TRANSPORT_FRAME_READY);

    /* Encoding supports payload storage immediately after the header. */
    memcpy(frame + KRUL_TRANSPORT_HEADER_SIZE, json, sizeof(json) - 1U);
    frame_size = krul_transport_encode(
        KRUL_TRANSPORT_FORMAT_JSON, frame + KRUL_TRANSPORT_HEADER_SIZE,
        sizeof(json) - 1U, frame, sizeof(frame));
    assert(frame_size > 0U && memcmp(frame, "KRJ1", 4U) == 0);

    puts("krul_transport_test: OK");
    return 0;
}
