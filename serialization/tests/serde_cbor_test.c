#include "serde_cbor.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define KEY(name_, tag_) (&(const serde_key_t){.name = (name_), .tag = (tag_)})

static serde_node_t member(serde_codec_t codec, serde_node_t object,
                           const char* name, uint16_t tag) {
    serde_node_t value = {0};
    size_t matches = 0U;
    assert(serde_object_get(codec, object, KEY(name, tag), &value, &matches));
    assert(matches == 1U);
    return value;
}

int main(void) {
    serde_cbor_codec_t cbor;
    assert(serde_cbor_init(&cbor));
    serde_codec_t codec = serde_cbor(&cbor);
    uint8_t output[256];
    serde_writer_storage_t storage;
    serde_writer_t writer;
    assert(serde_writer_open(codec, &storage, output, sizeof(output), &writer));
    assert(serde_begin_object(writer, NULL));
    assert(serde_put_i32(writer, KEY("i", 1U), -7));
    assert(serde_put_u32(writer, KEY("u", 2U), UINT32_MAX));
    assert(serde_put_f32(writer, KEY("f", 3U), 1.25F));
    assert(serde_put_bool(writer, KEY("b", 4U), true));
    assert(serde_put_string(writer, KEY("s", 5U), "hello"));
    assert(serde_put_null(writer, KEY("n", 6U)));
    assert(serde_begin_array(writer, KEY("a", 7U)));
    assert(serde_put_i32(writer, NULL, 1));
    assert(serde_put_string(writer, NULL, "two"));
    assert(serde_end_array(writer));
    assert(serde_end_object(writer));
    size_t encoded_size = 0U;
    assert(serde_writer_finish(writer, &encoded_size));
    assert(output[0] == 0xbfU && output[encoded_size - 1U] == 0xffU);

    serde_node_t root = {0};
    assert(serde_decode(codec, output, encoded_size, &root) == SERDE_OK);
    assert(serde_kind(codec, root) == SERDE_KIND_OBJECT);
    assert(serde_object_size(codec, root) == 7U);
    int32_t i = 0;
    uint32_t u = 0U;
    float f = 0.0F;
    bool b = false;
    char text[16];
    assert(serde_get_i32(codec, member(codec, root, "i", 1U), &i) && i == -7);
    assert(serde_get_u32(codec, member(codec, root, "u", 2U), &u) &&
           u == UINT32_MAX);
    assert(serde_get_f32(codec, member(codec, root, "f", 3U), &f) &&
           fabsf(f - 1.25F) < 0.0001F);
    assert(serde_get_bool(codec, member(codec, root, "b", 4U), &b) && b);
    assert(serde_get_string(codec, member(codec, root, "s", 5U), text,
                            sizeof(text), NULL) &&
           strcmp(text, "hello") == 0);
    assert(serde_kind(codec, member(codec, root, "n", 6U)) == SERDE_KIND_NULL);
    serde_node_t array = member(codec, root, "a", 7U);
    assert(serde_array_size(codec, array) == 2U);
    serde_node_t item = {0};
    assert(serde_array_get(codec, array, 1U, &item));
    assert(serde_get_string(codec, item, text, sizeof(text), NULL));
    assert(strcmp(text, "two") == 0);

    static const uint8_t definite[] = {0xa1U, 0x18U, 0x2aU, 0x19U, 0x03U,
                                       0xe8U};
    assert(serde_decode(codec, definite, sizeof(definite), &root) == SERDE_OK);
    assert(serde_get_u32(codec, member(codec, root, NULL, 42U), &u) &&
           u == 1000U);
    static const uint8_t malformed[] = {0xbfU, 0x01U, 0xffU};
    assert(serde_decode(codec, malformed, sizeof(malformed), &root) ==
           SERDE_ERROR_MALFORMED);
    puts("serde_cbor_test: OK");
    return 0;
}
