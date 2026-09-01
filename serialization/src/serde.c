#include "serde.h"

/*
 * Тонкий диспетчер интерфейса serde.
 *
 * В этом файле нет логики JSON. Функции проверяют наличие метода и передают
 * вызов конкретной реализации через mtab/self. Здесь же реализован общий поиск
 * поля объекта, одинаковый для текстовых имён и числовых tag.
 */

#include <string.h>

uint16_t serde_key_tag(const char* name) {
    if (name == NULL) return 0U;
    uint32_t hash = 5381U;
    const unsigned char* cursor = (const unsigned char*)name;
    while (*cursor != 0U) {
        hash = ((hash * 33U) ^ *cursor++) % 65521U;
    }
    return (uint16_t)(hash + 1U);
}

static bool key_matches(const serde_key_view_t* view, const serde_key_t* key) {
    /* При наличии tag он имеет приоритет; иначе сравниваем имя без создания
     * временной null-terminated строки. */
    if (view == NULL || key == NULL) return false;
    if (view->has_tag && key->tag != 0U && view->tag == key->tag) return true;
    if (!view->has_name || key->name == NULL) return false;
    size_t length = strlen(key->name);
    return length == view->name_length &&
           memcmp(view->name, key->name, length) == 0;
}

serde_status_t serde_decode(serde_codec_t codec, const uint8_t* data,
                            size_t size, serde_node_t* root) {
    if (codec.mtab == NULL || codec.mtab->decode == NULL) return SERDE_ERROR_STATE;
    return codec.mtab->decode(codec.self, data, size, root);
}

serde_kind_t serde_kind(serde_codec_t codec, serde_node_t node) {
    return codec.mtab != NULL && codec.mtab->kind != NULL
               ? codec.mtab->kind(codec.self, node)
               : SERDE_KIND_INVALID;
}

size_t serde_object_size(serde_codec_t codec, serde_node_t object) {
    return codec.mtab != NULL && codec.mtab->object_size != NULL
               ? codec.mtab->object_size(codec.self, object)
               : 0U;
}

bool serde_object_member(serde_codec_t codec, serde_node_t object,
                         size_t index, serde_key_view_t* key,
                         serde_node_t* value) {
    return codec.mtab != NULL && codec.mtab->object_member != NULL &&
           codec.mtab->object_member(codec.self, object, index, key, value);
}

bool serde_object_get(serde_codec_t codec, serde_node_t object,
                      const serde_key_t* key, serde_node_t* value,
                      size_t* matches) {
    /* Не останавливаемся после первого совпадения: вызывающему коду нужно знать
     * о дубликатах ключа в исходном объекте. */
    size_t count = 0U;
    size_t members = serde_object_size(codec, object);
    for (size_t index = 0U; index < members; ++index) {
        serde_key_view_t found_key = {0};
        serde_node_t found_value = {0};
        if (!serde_object_member(codec, object, index, &found_key,
                                 &found_value))
            return false;
        if (key_matches(&found_key, key)) {
            if (count == 0U && value != NULL) *value = found_value;
            ++count;
        }
    }
    if (matches != NULL) *matches = count;
    return count > 0U;
}

size_t serde_array_size(serde_codec_t codec, serde_node_t array) {
    return codec.mtab != NULL && codec.mtab->array_size != NULL
               ? codec.mtab->array_size(codec.self, array)
               : 0U;
}

bool serde_array_get(serde_codec_t codec, serde_node_t array, size_t index,
                     serde_node_t* value) {
    return codec.mtab != NULL && codec.mtab->array_get != NULL &&
           codec.mtab->array_get(codec.self, array, index, value);
}

#define SERDE_GET_WRAPPER(_name, _type)                                  \
    bool serde_get_##_name(serde_codec_t codec, serde_node_t node,      \
                           _type* value) {                                \
        return codec.mtab != NULL && codec.mtab->get_##_name != NULL && \
               codec.mtab->get_##_name(codec.self, node, value);       \
    }

SERDE_GET_WRAPPER(i32, int32_t)
SERDE_GET_WRAPPER(u32, uint32_t)
SERDE_GET_WRAPPER(f32, float)
SERDE_GET_WRAPPER(bool, bool)

/* Макросы выше/ниже создают только безопасные forwarding-функции и сохраняют
 * единый публичный API для всех реализаций кодека. */

bool serde_get_string(serde_codec_t codec, serde_node_t node, char* output,
                      size_t capacity, size_t* length) {
    return codec.mtab != NULL && codec.mtab->get_string != NULL &&
           codec.mtab->get_string(codec.self, node, output, capacity,
                                   length);
}

bool serde_writer_open(serde_codec_t codec,
                       serde_writer_storage_t* storage, uint8_t* output,
                       size_t capacity, serde_writer_t* writer) {
    return codec.mtab != NULL && codec.mtab->writer_open != NULL &&
           codec.mtab->writer_open(codec.self, storage, output, capacity,
                                    writer);
}

#define SERDE_WRITER_CONTAINER(_name)                                    \
    bool serde_##_name(serde_writer_t writer, const serde_key_t* key) {   \
        return writer.mtab != NULL && writer.mtab->_name != NULL &&       \
               writer.mtab->_name(writer.self, key);                      \
    }

SERDE_WRITER_CONTAINER(begin_object)
SERDE_WRITER_CONTAINER(begin_array)

bool serde_end_object(serde_writer_t writer) {
    return writer.mtab != NULL && writer.mtab->end_object != NULL &&
           writer.mtab->end_object(writer.self);
}

bool serde_end_array(serde_writer_t writer) {
    return writer.mtab != NULL && writer.mtab->end_array != NULL &&
           writer.mtab->end_array(writer.self);
}

#define SERDE_PUT_WRAPPER(_name, _type)                                  \
    bool serde_put_##_name(serde_writer_t writer, const serde_key_t* key, \
                           _type value) {                                 \
        return writer.mtab != NULL && writer.mtab->put_##_name != NULL && \
               writer.mtab->put_##_name(writer.self, key, value);         \
    }

SERDE_PUT_WRAPPER(i32, int32_t)
SERDE_PUT_WRAPPER(u32, uint32_t)
SERDE_PUT_WRAPPER(f32, float)
SERDE_PUT_WRAPPER(bool, bool)

bool serde_put_string_n(serde_writer_t writer, const serde_key_t* key,
                        const char* value, size_t length) {
    return writer.mtab != NULL && writer.mtab->put_string != NULL &&
           value != NULL &&
           writer.mtab->put_string(writer.self, key, value, length);
}

bool serde_put_string(serde_writer_t writer, const serde_key_t* key, const char* value) {
    return value != NULL && serde_put_string_n(writer, key, value, strlen(value));
}

bool serde_put_null(serde_writer_t writer, const serde_key_t* key) {
    return writer.mtab != NULL && writer.mtab->put_null != NULL &&
           writer.mtab->put_null(writer.self, key);
}

bool serde_writer_finish(serde_writer_t writer, size_t* encoded_size) {
    return writer.mtab != NULL && writer.mtab->finish != NULL &&
           writer.mtab->finish(writer.self, encoded_size);
}

bool serde_writer_ok(serde_writer_t writer) {
    return writer.mtab != NULL && writer.mtab->ok != NULL &&
           writer.mtab->ok(writer.self);
}
