#include "serde_bson.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define KEY(_name) (&(const serde_key_t){.name = (_name)})

static serde_node_t member(serde_codec_t codec, serde_node_t object,
                           const char* name) {
    serde_node_t value = {0};
    size_t matches = 0U;
    assert(serde_object_get(codec, object, KEY(name), &value, &matches));
    assert(matches == 1U);
    return value;
}

int main(void) {
    serde_bson_codec_t bson;
    assert(serde_bson_init(&bson));
    serde_codec_t codec = serde_bson(&bson);

    uint8_t output[512];
    serde_writer_storage_t storage;
    serde_writer_t writer;
    assert(serde_writer_open(codec, &storage, output, sizeof(output), &writer));
    assert(serde_begin_object(writer, NULL));
    assert(serde_put_i32(writer, KEY("i"), -7));
    assert(serde_put_u32(writer, KEY("u"), UINT32_C(4000000000)));
    assert(serde_put_f32(writer, KEY("f"), 1.5F));
    assert(serde_put_bool(writer, KEY("b"), true));
    assert(serde_put_string(writer, KEY("s"), "hi"));
    assert(serde_put_null(writer, KEY("n")));
    assert(serde_begin_object(writer, KEY("o")));
    assert(serde_put_i32(writer, KEY("x"), 9));
    assert(serde_end_object(writer));
    assert(serde_begin_array(writer, KEY("a")));
    assert(serde_put_i32(writer, NULL, 1));
    assert(serde_put_string(writer, NULL, "z"));
    assert(serde_end_array(writer));
    assert(serde_end_object(writer));
    size_t encoded_size = 0U;
    assert(serde_writer_finish(writer, &encoded_size));

    serde_node_t root = {0};
    assert(serde_decode(codec, output, encoded_size, &root) == SERDE_OK);
    assert(serde_kind(codec, root) == SERDE_KIND_OBJECT);
    assert(serde_object_size(codec, root) == 8U);

    int32_t i = 0;
    uint32_t u = 0U;
    float f = 0.0F;
    bool b = false;
    char text[8];
    size_t text_length = 0U;
    assert(serde_get_i32(codec, member(codec, root, "i"), &i) && i == -7);
    assert(serde_get_u32(codec, member(codec, root, "u"), &u) &&
           u == UINT32_C(4000000000));
    assert(serde_get_f32(codec, member(codec, root, "f"), &f) &&
           fabsf(f - 1.5F) < 0.0001F);
    assert(serde_get_bool(codec, member(codec, root, "b"), &b) && b);
    assert(serde_get_string(codec, member(codec, root, "s"), text,
                            sizeof(text), &text_length));
    assert(text_length == 2U && strcmp(text, "hi") == 0);
    assert(serde_kind(codec, member(codec, root, "n")) == SERDE_KIND_NULL);

    serde_node_t object = member(codec, root, "o");
    assert(serde_kind(codec, object) == SERDE_KIND_OBJECT);
    assert(serde_get_i32(codec, member(codec, object, "x"), &i) && i == 9);
    serde_node_t array = member(codec, root, "a");
    assert(serde_kind(codec, array) == SERDE_KIND_ARRAY);
    assert(serde_array_size(codec, array) == 2U);
    serde_node_t item = {0};
    assert(serde_array_get(codec, array, 0U, &item));
    assert(serde_get_i32(codec, item, &i) && i == 1);
    assert(serde_array_get(codec, array, 1U, &item));
    assert(serde_get_string(codec, item, text, sizeof(text), NULL));
    assert(strcmp(text, "z") == 0);

    static const uint8_t canonical[] = {
        12, 0, 0, 0, 0x10, 'x', 0, 42, 0, 0, 0, 0};
    assert(serde_writer_open(codec, &storage, output, sizeof(output), &writer));
    assert(serde_begin_object(writer, NULL));
    assert(serde_put_i32(writer, KEY("x"), 42));
    assert(serde_end_object(writer));
    assert(serde_writer_finish(writer, &encoded_size));
    assert(encoded_size == sizeof(canonical));
    assert(memcmp(output, canonical, sizeof(canonical)) == 0);
    assert(serde_decode(codec, canonical, sizeof(canonical), &root) == SERDE_OK);
    assert(serde_get_i32(codec, member(codec, root, "x"), &i) && i == 42);

    /* Unknown but structurally valid BSON values remain skippable extensions. */
    static const uint8_t binary_extension[] = {
        15, 0, 0, 0, 0x05, 'x', 0, 2, 0, 0, 0, 0, 1, 2, 0};
    assert(serde_decode(codec, binary_extension, sizeof(binary_extension),
                        &root) == SERDE_OK);
    assert(serde_kind(codec, member(codec, root, "x")) == SERDE_KIND_INVALID);

    uint8_t malformed[sizeof(canonical)];
    memcpy(malformed, canonical, sizeof(malformed));
    malformed[0] = 11U;
    assert(serde_decode(codec, malformed, sizeof(malformed), &root) ==
           SERDE_ERROR_MALFORMED);

    /* BSON arrays must use canonical sequential decimal field names. */
    static const uint8_t bad_array[] = {
        20, 0, 0, 0, 0x04, 'a', 0, 12, 0, 0, 0,
        0x10, '1', 0, 1, 0, 0, 0, 0, 0};
    assert(serde_decode(codec, bad_array, sizeof(bad_array), &root) ==
           SERDE_ERROR_MALFORMED);

    uint8_t tiny[10];
    assert(serde_writer_open(codec, &storage, tiny, sizeof(tiny), &writer));
    assert(serde_begin_object(writer, NULL));
    assert(!serde_put_i32(writer, KEY("x"), 42));
    assert(!serde_writer_ok(writer));
    assert(!serde_writer_finish(writer, NULL));

    puts("serde_bson_test: OK");
    return 0;
}
