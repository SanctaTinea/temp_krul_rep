#include "serde_bson.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#define BSON_MAX_DEPTH 12U

enum {
    BSON_DOUBLE = 0x01,
    BSON_STRING = 0x02,
    BSON_DOCUMENT = 0x03,
    BSON_ARRAY = 0x04,
    BSON_BINARY = 0x05,
    BSON_UNDEFINED = 0x06,
    BSON_OBJECT_ID = 0x07,
    BSON_BOOL = 0x08,
    BSON_DATETIME = 0x09,
    BSON_NULL = 0x0A,
    BSON_REGEX = 0x0B,
    BSON_DB_POINTER = 0x0C,
    BSON_JAVASCRIPT = 0x0D,
    BSON_SYMBOL = 0x0E,
    BSON_CODE_SCOPE = 0x0F,
    BSON_INT32 = 0x10,
    BSON_TIMESTAMP = 0x11,
    BSON_INT64 = 0x12,
    BSON_DECIMAL128 = 0x13,
    BSON_MAX_KEY = 0x7F,
    BSON_MIN_KEY = 0xFF
};

typedef struct {
    uint32_t start;
    uint32_t next_index;
    uint8_t kind;
} bson_level_t;

typedef struct {
    uint8_t* buffer;
    uint32_t capacity;
    uint32_t length;
    bool failed;
    bool root_finished;
    uint8_t depth;
    bson_level_t levels[BSON_MAX_DEPTH];
} bson_writer_impl_t;

_Static_assert(sizeof(bson_writer_impl_t) <= SERDE_WRITER_STORAGE_SIZE,
               "SERDE_WRITER_STORAGE_SIZE is too small for BSON");

static uint32_t read_u32(const uint8_t* value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static uint64_t read_u64(const uint8_t* value) {
    uint64_t result = 0U;
    for (unsigned int index = 0U; index < 8U; ++index)
        result |= (uint64_t)value[index] << (index * 8U);
    return result;
}

static void write_u32(uint8_t* output, uint32_t value) {
    for (unsigned int index = 0U; index < 4U; ++index)
        output[index] = (uint8_t)(value >> (index * 8U));
}

static void write_u64(uint8_t* output, uint64_t value) {
    for (unsigned int index = 0U; index < 8U; ++index)
        output[index] = (uint8_t)(value >> (index * 8U));
}

static bool span_fits(size_t limit, size_t offset, size_t length) {
    return offset <= limit && length <= limit - offset;
}

static bool find_cstring(const uint8_t* input, size_t limit, size_t offset,
                         size_t* length) {
    if (offset >= limit) return false;
    const uint8_t* end = memchr(input + offset, 0, limit - offset);
    if (end == NULL) return false;
    if (length != NULL) *length = (size_t)(end - (input + offset));
    return true;
}

static bool validate_document(const uint8_t* input, size_t input_size,
                              size_t start, unsigned int depth, bool array,
                              size_t* document_end);

static bool string_end(const uint8_t* input, size_t limit, size_t start,
                       size_t* end) {
    if (!span_fits(limit, start, 4U)) return false;
    int32_t length = (int32_t)read_u32(input + start);
    if (length < 1 || !span_fits(limit, start + 4U, (size_t)length) ||
        input[start + 4U + (size_t)length - 1U] != 0U)
        return false;
    *end = start + 4U + (size_t)length;
    return true;
}

static bool validate_value(const uint8_t* input, size_t input_size,
                           size_t limit, uint8_t type, size_t start,
                           unsigned int depth, size_t* end) {
    size_t first_end = 0U;
    switch (type) {
        case BSON_DOUBLE:
        case BSON_DATETIME:
        case BSON_TIMESTAMP:
        case BSON_INT64:
            if (!span_fits(limit, start, 8U)) return false;
            *end = start + 8U;
            return true;
        case BSON_STRING:
        case BSON_JAVASCRIPT:
        case BSON_SYMBOL:
            return string_end(input, limit, start, end);
        case BSON_DOCUMENT:
        case BSON_ARRAY:
            return validate_document(input, input_size, start, depth + 1U,
                                     type == BSON_ARRAY, end) && *end <= limit;
        case BSON_BINARY: {
            if (!span_fits(limit, start, 5U)) return false;
            int32_t length = (int32_t)read_u32(input + start);
            if (length < 0 || !span_fits(limit, start + 5U, (size_t)length))
                return false;
            *end = start + 5U + (size_t)length;
            return true;
        }
        case BSON_UNDEFINED:
        case BSON_NULL:
        case BSON_MAX_KEY:
        case BSON_MIN_KEY:
            *end = start;
            return true;
        case BSON_OBJECT_ID:
            if (!span_fits(limit, start, 12U)) return false;
            *end = start + 12U;
            return true;
        case BSON_BOOL:
            if (!span_fits(limit, start, 1U) || input[start] > 1U) return false;
            *end = start + 1U;
            return true;
        case BSON_REGEX: {
            size_t length = 0U;
            if (!find_cstring(input, limit, start, &length)) return false;
            first_end = start + length + 1U;
            if (!find_cstring(input, limit, first_end, &length)) return false;
            *end = first_end + length + 1U;
            return true;
        }
        case BSON_DB_POINTER:
            if (!string_end(input, limit, start, &first_end) ||
                !span_fits(limit, first_end, 12U))
                return false;
            *end = first_end + 12U;
            return true;
        case BSON_CODE_SCOPE: {
            if (!span_fits(limit, start, 4U)) return false;
            int32_t total = (int32_t)read_u32(input + start);
            if (total < 14 || !span_fits(limit, start, (size_t)total) ||
                !string_end(input, start + (size_t)total, start + 4U,
                            &first_end))
                return false;
            size_t scope_end = 0U;
            if (!validate_document(input, input_size, first_end, depth + 1U,
                                   false, &scope_end) ||
                scope_end != start + (size_t)total)
                return false;
            *end = scope_end;
            return true;
        }
        case BSON_INT32:
            if (!span_fits(limit, start, 4U)) return false;
            *end = start + 4U;
            return true;
        case BSON_DECIMAL128:
            if (!span_fits(limit, start, 16U)) return false;
            *end = start + 16U;
            return true;
        default:
            return false;
    }
}

static bool array_key_matches(const uint8_t* key, size_t length,
                              size_t wanted) {
    char expected[21];
    size_t count = 0U;
    do {
        expected[count++] = (char)('0' + wanted % 10U);
        wanted /= 10U;
    } while (wanted != 0U);
    if (count != length) return false;
    for (size_t index = 0U; index < count; ++index)
        if ((uint8_t)expected[count - index - 1U] != key[index]) return false;
    return true;
}

static bool validate_document(const uint8_t* input, size_t input_size,
                              size_t start, unsigned int depth, bool array,
                              size_t* document_end) {
    if (depth >= BSON_MAX_DEPTH || !span_fits(input_size, start, 5U))
        return false;
    int32_t length = (int32_t)read_u32(input + start);
    if (length < 5 || !span_fits(input_size, start, (size_t)length))
        return false;
    size_t end = start + (size_t)length;
    if (input[end - 1U] != 0U) return false;
    size_t offset = start + 4U;
    size_t array_index = 0U;
    while (offset < end - 1U) {
        uint8_t type = input[offset++];
        if (type == 0U) return false;
        size_t key_length = 0U;
        if (!find_cstring(input, end - 1U, offset, &key_length)) return false;
        if (array && !array_key_matches(input + offset, key_length,
                                        array_index++))
            return false;
        offset += key_length + 1U;
        if (!validate_value(input, input_size, end - 1U, type, offset, depth,
                            &offset))
            return false;
    }
    if (offset != end - 1U) return false;
    *document_end = end;
    return true;
}

static serde_status_t bson_decode(void* self, const uint8_t* data, size_t size,
                                  serde_node_t* root) {
    serde_bson_codec_t* bson = self;
    if (bson == NULL || !bson->initialized || data == NULL || root == NULL)
        return SERDE_ERROR_STATE;
    if (size > UINT32_MAX) return SERDE_ERROR_NO_SPACE;
    size_t end = 0U;
    if (!validate_document(data, size, 0U, 0U, false, &end) || end != size) {
        bson->input = NULL;
        bson->input_size = 0U;
        return SERDE_ERROR_MALFORMED;
    }
    bson->input = data;
    bson->input_size = size;
    *root = (serde_node_t){.value = 0U};
    return SERDE_OK;
}

static bool node_parts(const serde_bson_codec_t* bson, serde_node_t node,
                       uint8_t* type, size_t* value_offset) {
    if (bson == NULL || bson->input == NULL || node.value == 0U ||
        node.value >= bson->input_size)
        return false;
    size_t key_length = 0U;
    if (!find_cstring(bson->input, bson->input_size, node.value + 1U,
                      &key_length))
        return false;
    *type = bson->input[node.value];
    *value_offset = (size_t)node.value + 1U + key_length + 1U;
    return *value_offset <= bson->input_size;
}

static serde_kind_t bson_kind(const void* self, serde_node_t node) {
    const serde_bson_codec_t* bson = self;
    if (bson == NULL || bson->input == NULL) return SERDE_KIND_INVALID;
    if (node.value == 0U) return SERDE_KIND_OBJECT;
    uint8_t type = 0U;
    size_t unused = 0U;
    if (!node_parts(bson, node, &type, &unused)) return SERDE_KIND_INVALID;
    switch (type) {
        case BSON_NULL: return SERDE_KIND_NULL;
        case BSON_DOCUMENT: return SERDE_KIND_OBJECT;
        case BSON_ARRAY: return SERDE_KIND_ARRAY;
        case BSON_STRING: return SERDE_KIND_STRING;
        case BSON_DOUBLE:
        case BSON_INT32:
        case BSON_INT64: return SERDE_KIND_NUMBER;
        case BSON_BOOL: return SERDE_KIND_BOOL;
        default: return SERDE_KIND_INVALID;
    }
}

static bool document_range(const serde_bson_codec_t* bson, serde_node_t node,
                           uint8_t expected_type, size_t* start, size_t* end) {
    if (bson == NULL || bson->input == NULL) return false;
    if (node.value == 0U) {
        if (expected_type != BSON_DOCUMENT) return false;
        *start = 0U;
    } else {
        uint8_t type = 0U;
        if (!node_parts(bson, node, &type, start) || type != expected_type)
            return false;
    }
    int32_t length = (int32_t)read_u32(bson->input + *start);
    *end = *start + (size_t)length;
    return true;
}

static bool element_at(const serde_bson_codec_t* bson, serde_node_t document,
                       uint8_t document_type, size_t wanted,
                       serde_key_view_t* key, serde_node_t* value) {
    size_t start = 0U;
    size_t end = 0U;
    if (!document_range(bson, document, document_type, &start, &end))
        return false;
    size_t offset = start + 4U;
    size_t index = 0U;
    while (offset < end - 1U) {
        size_t type_offset = offset++;
        size_t key_length = 0U;
        if (!find_cstring(bson->input, end - 1U, offset, &key_length))
            return false;
        size_t value_offset = offset + key_length + 1U;
        size_t next = 0U;
        if (!validate_value(bson->input, bson->input_size, end - 1U,
                            bson->input[type_offset], value_offset, 0U, &next))
            return false;
        if (index++ == wanted) {
            if (key != NULL)
                *key = (serde_key_view_t){.name = (const char*)bson->input + offset,
                                          .name_length = key_length,
                                          .has_name = true};
            if (value != NULL)
                *value = (serde_node_t){.value = (uint32_t)type_offset};
            return true;
        }
        offset = next;
    }
    return false;
}

static size_t bson_container_size(const serde_bson_codec_t* bson,
                                  serde_node_t node, uint8_t type) {
    size_t start = 0U;
    size_t end = 0U;
    if (!document_range(bson, node, type, &start, &end)) return 0U;
    size_t count = 0U;
    size_t offset = start + 4U;
    while (offset < end - 1U) {
        size_t type_offset = offset++;
        size_t key_length = 0U;
        if (!find_cstring(bson->input, end - 1U, offset, &key_length)) return 0U;
        offset += key_length + 1U;
        if (!validate_value(bson->input, bson->input_size, end - 1U,
                            bson->input[type_offset], offset, 0U, &offset))
            return 0U;
        ++count;
    }
    return count;
}

static size_t bson_object_size(const void* self, serde_node_t object) {
    return bson_container_size(self, object, BSON_DOCUMENT);
}

static bool bson_object_member(const void* self, serde_node_t object,
                               size_t index, serde_key_view_t* key,
                               serde_node_t* value) {
    if (key == NULL || value == NULL) return false;
    return element_at(self, object, BSON_DOCUMENT, index, key, value);
}

static size_t bson_array_size(const void* self, serde_node_t array) {
    return bson_container_size(self, array, BSON_ARRAY);
}

static bool bson_array_get(const void* self, serde_node_t array, size_t index,
                           serde_node_t* value) {
    if (value == NULL) return false;
    return element_at(self, array, BSON_ARRAY, index, NULL, value);
}

static bool bson_number(const serde_bson_codec_t* bson, serde_node_t node,
                        uint8_t* type, uint64_t* bits) {
    size_t offset = 0U;
    if (!node_parts(bson, node, type, &offset)) return false;
    if (*type == BSON_INT32) *bits = read_u32(bson->input + offset);
    else if (*type == BSON_INT64 || *type == BSON_DOUBLE)
        *bits = read_u64(bson->input + offset);
    else return false;
    return true;
}

static bool bson_get_i32(const void* self, serde_node_t node, int32_t* output) {
    uint8_t type = 0U;
    uint64_t bits = 0U;
    if (output == NULL || !bson_number(self, node, &type, &bits) ||
        type == BSON_DOUBLE)
        return false;
    int64_t value = type == BSON_INT32 ? (int32_t)(uint32_t)bits
                                       : (int64_t)bits;
    if (value < INT32_MIN || value > INT32_MAX) return false;
    *output = (int32_t)value;
    return true;
}

static bool bson_get_u32(const void* self, serde_node_t node,
                         uint32_t* output) {
    uint8_t type = 0U;
    uint64_t bits = 0U;
    if (output == NULL || !bson_number(self, node, &type, &bits) ||
        type == BSON_DOUBLE)
        return false;
    int64_t value = type == BSON_INT32 ? (int32_t)(uint32_t)bits
                                       : (int64_t)bits;
    if (value < 0 || (uint64_t)value > UINT32_MAX) return false;
    *output = (uint32_t)value;
    return true;
}

static bool bson_get_f32(const void* self, serde_node_t node, float* output) {
    uint8_t type = 0U;
    uint64_t bits = 0U;
    if (output == NULL || !bson_number(self, node, &type, &bits)) return false;
    double value = 0.0;
    if (type == BSON_DOUBLE) memcpy(&value, &bits, sizeof(value));
    else if (type == BSON_INT32) value = (double)(int32_t)(uint32_t)bits;
    else value = (double)(int64_t)bits;
    if (!isfinite(value) || value < -FLT_MAX || value > FLT_MAX) return false;
    *output = (float)value;
    return isfinite(*output);
}

static bool bson_get_bool(const void* self, serde_node_t node, bool* output) {
    const serde_bson_codec_t* bson = self;
    uint8_t type = 0U;
    size_t offset = 0U;
    if (output == NULL || !node_parts(bson, node, &type, &offset) ||
        type != BSON_BOOL)
        return false;
    *output = bson->input[offset] != 0U;
    return true;
}

static bool bson_get_string(const void* self, serde_node_t node, char* output,
                            size_t capacity, size_t* length) {
    const serde_bson_codec_t* bson = self;
    uint8_t type = 0U;
    size_t offset = 0U;
    if (!node_parts(bson, node, &type, &offset) || type != BSON_STRING ||
        (output != NULL && capacity == 0U))
        return false;
    size_t end = 0U;
    if (!string_end(bson->input, bson->input_size, offset, &end)) return false;
    size_t string_length = end - offset - 5U;
    if (output != NULL) {
        if (string_length + 1U > capacity) return false;
        memcpy(output, bson->input + offset + 4U, string_length);
        output[string_length] = '\0';
    }
    if (length != NULL) *length = string_length;
    return true;
}

static bool append_bytes(bson_writer_impl_t* writer, const void* data,
                         size_t length) {
    if (writer->failed) return false;
    if (length > writer->capacity - writer->length) {
        writer->failed = true;
        return false;
    }
    memcpy(writer->buffer + writer->length, data, length);
    writer->length += (uint32_t)length;
    return true;
}

static bool append_byte(bson_writer_impl_t* writer, uint8_t value) {
    return append_bytes(writer, &value, 1U);
}

static bool append_u32(bson_writer_impl_t* writer, uint32_t value) {
    uint8_t bytes[4];
    write_u32(bytes, value);
    return append_bytes(writer, bytes, sizeof(bytes));
}

static bool append_u64(bson_writer_impl_t* writer, uint64_t value) {
    uint8_t bytes[8];
    write_u64(bytes, value);
    return append_bytes(writer, bytes, sizeof(bytes));
}

static bool append_array_key(bson_writer_impl_t* writer, uint32_t value) {
    char reversed[10];
    size_t count = 0U;
    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count > 0U)
        if (!append_byte(writer, (uint8_t)reversed[--count])) return false;
    return append_byte(writer, 0U);
}

static bool element_header(bson_writer_impl_t* writer,
                           const serde_key_t* key, uint8_t type) {
    if (writer->failed || writer->depth == 0U) {
        writer->failed = true;
        return false;
    }
    bson_level_t* level = &writer->levels[writer->depth - 1U];
    if (!append_byte(writer, type)) return false;
    if (level->kind == BSON_DOCUMENT) {
        if (key == NULL || key->name == NULL ||
            !append_bytes(writer, key->name, strlen(key->name)) ||
            !append_byte(writer, 0U)) {
            writer->failed = true;
            return false;
        }
    } else {
        if (key != NULL || !append_array_key(writer, level->next_index++)) {
            writer->failed = true;
            return false;
        }
    }
    return true;
}

static bool bson_begin_container(void* self, const serde_key_t* key,
                                 uint8_t kind) {
    bson_writer_impl_t* writer = self;
    if (writer->failed || writer->depth >= BSON_MAX_DEPTH ||
        writer->root_finished) {
        writer->failed = true;
        return false;
    }
    if (writer->depth == 0U) {
        if (writer->length != 0U || key != NULL || kind != BSON_DOCUMENT) {
            writer->failed = true;
            return false;
        }
    } else if (!element_header(writer, key, kind)) {
        return false;
    }
    uint32_t start = writer->length;
    if (!append_u32(writer, 0U)) return false;
    writer->levels[writer->depth++] =
        (bson_level_t){.start = start, .kind = kind};
    return true;
}

static bool bson_begin_object(void* self, const serde_key_t* key) {
    return bson_begin_container(self, key, BSON_DOCUMENT);
}

static bool bson_begin_array(void* self, const serde_key_t* key) {
    return bson_begin_container(self, key, BSON_ARRAY);
}

static bool bson_end_container(void* self, uint8_t kind) {
    bson_writer_impl_t* writer = self;
    if (writer->failed || writer->depth == 0U ||
        writer->levels[writer->depth - 1U].kind != kind ||
        !append_byte(writer, 0U)) {
        writer->failed = true;
        return false;
    }
    bson_level_t level = writer->levels[--writer->depth];
    write_u32(writer->buffer + level.start, writer->length - level.start);
    if (writer->depth == 0U) writer->root_finished = true;
    return true;
}

static bool bson_end_object(void* self) {
    return bson_end_container(self, BSON_DOCUMENT);
}

static bool bson_end_array(void* self) {
    return bson_end_container(self, BSON_ARRAY);
}

static bool bson_put_i32(void* self, const serde_key_t* key, int32_t value) {
    bson_writer_impl_t* writer = self;
    return element_header(writer, key, BSON_INT32) &&
           append_u32(writer, (uint32_t)value);
}

static bool bson_put_u32(void* self, const serde_key_t* key, uint32_t value) {
    bson_writer_impl_t* writer = self;
    if (value <= INT32_MAX)
        return bson_put_i32(self, key, (int32_t)value);
    return element_header(writer, key, BSON_INT64) &&
           append_u64(writer, value);
}

static bool bson_put_f32(void* self, const serde_key_t* key, float value) {
    bson_writer_impl_t* writer = self;
    if (!isfinite(value) || !element_header(writer, key, BSON_DOUBLE)) {
        writer->failed = true;
        return false;
    }
    double encoded = value;
    uint64_t bits = 0U;
    memcpy(&bits, &encoded, sizeof(bits));
    return append_u64(writer, bits);
}

static bool bson_put_bool(void* self, const serde_key_t* key, bool value) {
    bson_writer_impl_t* writer = self;
    return element_header(writer, key, BSON_BOOL) &&
           append_byte(writer, value ? 1U : 0U);
}

static bool bson_put_string(void* self, const serde_key_t* key,
                            const char* value, size_t length) {
    bson_writer_impl_t* writer = self;
    if (length > INT32_MAX - 1U || memchr(value, 0, length) != NULL ||
        !element_header(writer, key, BSON_STRING) ||
        !append_u32(writer, (uint32_t)length + 1U) ||
        !append_bytes(writer, value, length) || !append_byte(writer, 0U)) {
        writer->failed = true;
        return false;
    }
    return true;
}

static bool bson_put_null(void* self, const serde_key_t* key) {
    return element_header(self, key, BSON_NULL);
}

static bool bson_finish(void* self, size_t* encoded_size) {
    bson_writer_impl_t* writer = self;
    if (writer->failed || writer->depth != 0U || !writer->root_finished ||
        writer->length < 5U) {
        writer->failed = true;
        return false;
    }
    if (encoded_size != NULL) *encoded_size = writer->length;
    return true;
}

static bool bson_ok(const void* self) {
    const bson_writer_impl_t* writer = self;
    return writer != NULL && !writer->failed;
}

static const serde_writer_mtab_t bson_writer_mtab = {
    .begin_object = bson_begin_object,
    .end_object = bson_end_object,
    .begin_array = bson_begin_array,
    .end_array = bson_end_array,
    .put_i32 = bson_put_i32,
    .put_u32 = bson_put_u32,
    .put_f32 = bson_put_f32,
    .put_bool = bson_put_bool,
    .put_string = bson_put_string,
    .put_null = bson_put_null,
    .finish = bson_finish,
    .ok = bson_ok};

static bool bson_writer_open(void* self, serde_writer_storage_t* storage,
                             uint8_t* output, size_t capacity,
                             serde_writer_t* writer) {
    serde_bson_codec_t* bson = self;
    if (bson == NULL || !bson->initialized || storage == NULL ||
        output == NULL || capacity < 5U || capacity > INT32_MAX ||
        writer == NULL)
        return false;
    bson_writer_impl_t* impl = (bson_writer_impl_t*)storage->bytes;
    *impl = (bson_writer_impl_t){.buffer = output,
                                  .capacity = (uint32_t)capacity};
    *writer = (serde_writer_t){.mtab = &bson_writer_mtab, .self = impl};
    return true;
}

static const serde_codec_mtab_t bson_codec_mtab = {
    .decode = bson_decode,
    .kind = bson_kind,
    .object_size = bson_object_size,
    .object_member = bson_object_member,
    .array_size = bson_array_size,
    .array_get = bson_array_get,
    .get_i32 = bson_get_i32,
    .get_u32 = bson_get_u32,
    .get_f32 = bson_get_f32,
    .get_bool = bson_get_bool,
    .get_string = bson_get_string,
    .writer_open = bson_writer_open};

bool serde_bson_init(serde_bson_codec_t* bson) {
    if (bson == NULL) return false;
    *bson = (serde_bson_codec_t){.initialized = true};
    return true;
}

serde_codec_t serde_bson(serde_bson_codec_t* bson) {
    if (bson == NULL || !bson->initialized) return (serde_codec_t){0};
    return (serde_codec_t){.mtab = &bson_codec_mtab, .self = bson};
}
