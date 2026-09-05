#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "serde_json.h"

#define ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))
#define KEY(value) (&(const serde_key_t){.name = (value)})

static serde_node_t member(serde_codec_t codec, serde_node_t object,
                           const char* name, size_t* matches) {
    serde_node_t value = {0};
    assert(serde_object_get(codec, object, KEY(name), &value, matches));
    return value;
}

static void test_decode_and_unicode(void) {
    serde_json_token_t tokens[32];
    serde_json_codec_t json;
    assert(serde_json_init(&json, tokens, ARRAY_SIZE(tokens)));
    serde_codec_t codec = serde_json(&json);
    static const uint8_t input[] = {
        0x7b, 0x22, 0x74, 0x65, 0x78, 0x74, 0x22, 0x3a, 0x22, 0x6c,
        0x69, 0x6e, 0x65, 0x5c, 0x6e, 0x5c, 0x75, 0x32, 0x30, 0x61,
        0x63, 0x5c, 0x75, 0x64, 0x38, 0x33, 0x64, 0x5c, 0x75, 0x64,
        0x65, 0x30, 0x30, 0x22, 0x2c, 0x22, 0x76, 0x61, 0x6c, 0x75,
        0x65, 0x22, 0x3a, 0x2d, 0x37, 0x7d};
    serde_node_t root;
    assert(serde_decode(codec, input, sizeof(input), &root) == SERDE_OK);

    char text[32];
    size_t length = 0U;
    assert(serde_get_string(codec, member(codec, root, "text", NULL), text,
                            sizeof(text), &length));
    static const uint8_t expected[] = {
        0x6c, 0x69, 0x6e, 0x65, 0x0a, 0xe2, 0x82, 0xac,
        0xf0, 0x9f, 0x98, 0x80};
    assert(length == sizeof(expected));
    assert(memcmp(text, expected, sizeof(expected)) == 0);

    int32_t value = 0;
    assert(serde_get_i32(codec, member(codec, root, "value", NULL), &value));
    assert(value == -7);
}

static void test_malformed_duplicate_and_token_limit(void) {
    serde_json_token_t tokens[16];
    serde_json_codec_t json;
    assert(serde_json_init(&json, tokens, ARRAY_SIZE(tokens)));
    serde_codec_t codec = serde_json(&json);
    serde_node_t root;

    static const uint8_t malformed[] = {0x7b, 0x62, 0x61, 0x64};
    assert(serde_decode(codec, malformed, sizeof(malformed), &root) ==
           SERDE_ERROR_MALFORMED);
    static const uint8_t trailing[] = {
        0x7b, 0x22, 0x78, 0x22, 0x3a, 0x31, 0x7d, 0x20, 0x78};
    assert(serde_decode(codec, trailing, sizeof(trailing), &root) ==
           SERDE_ERROR_MALFORMED);
    static const uint8_t bad_surrogate[] = {
        0x7b, 0x22, 0x74, 0x65, 0x78, 0x74, 0x22, 0x3a, 0x22,
        0x5c, 0x75, 0x64, 0x38, 0x30, 0x30, 0x22, 0x7d};
    assert(serde_decode(codec, bad_surrogate, sizeof(bad_surrogate), &root) ==
           SERDE_OK);
    char text[8];
    assert(!serde_get_string(codec, member(codec, root, "text", NULL), text,
                             sizeof(text), NULL));

    static const uint8_t duplicate[] = {
        0x7b, 0x22, 0x78, 0x22, 0x3a, 0x31, 0x2c,
        0x22, 0x78, 0x22, 0x3a, 0x32, 0x7d};
    assert(serde_decode(codec, duplicate, sizeof(duplicate), &root) == SERDE_OK);
    size_t matches = 0U;
    (void)member(codec, root, "x", &matches);
    assert(matches == 2U);

    serde_json_token_t tiny_tokens[3];
    assert(serde_json_init(&json, tiny_tokens, ARRAY_SIZE(tiny_tokens)));
    codec = serde_json(&json);
    static const uint8_t nested[] = {
        0x7b, 0x22, 0x61, 0x22, 0x3a, 0x5b, 0x31, 0x2c,
        0x32, 0x2c, 0x33, 0x5d, 0x7d};
    assert(serde_decode(codec, nested, sizeof(nested), &root) ==
           SERDE_ERROR_NO_SPACE);
}

static void test_writer_limits(void) {
    serde_json_token_t tokens[8];
    serde_json_codec_t json;
    assert(serde_json_init(&json, tokens, ARRAY_SIZE(tokens)));
    serde_codec_t codec = serde_json(&json);
    serde_writer_storage_t storage;
    serde_writer_t writer;
    uint8_t output[512];
    assert(serde_writer_open(codec, &storage, output, sizeof(output), &writer));
    assert(serde_begin_object(writer, NULL));
    for (size_t index = 0U; index < 11U; ++index)
        assert(serde_begin_object(writer, KEY("nested")));
    assert(!serde_begin_object(writer, KEY("too_deep")));
    assert(!serde_writer_ok(writer));

    uint8_t tiny_output[4];
    assert(serde_writer_open(codec, &storage, tiny_output, sizeof(tiny_output),
                             &writer));
    assert(serde_begin_object(writer, NULL));
    assert(!serde_put_string(writer, KEY("value"), "too long"));
    assert(!serde_writer_finish(writer, NULL));
}

int main(void) {
    test_decode_and_unicode();
    test_malformed_duplicate_and_token_limit();
    test_writer_limits();
    return 0;
}
