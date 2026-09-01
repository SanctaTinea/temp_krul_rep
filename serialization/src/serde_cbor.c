#include "serde_cbor.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define CBOR_MAX_DEPTH 16U
#define CBOR_INDEFINITE UINT64_MAX

typedef struct {
    uint8_t* output;
    size_t capacity;
    size_t size;
    uint8_t levels[CBOR_MAX_DEPTH];
    uint8_t depth;
    bool root_written;
    bool valid;
} cbor_writer_impl_t;

_Static_assert(sizeof(cbor_writer_impl_t) <= SERDE_WRITER_STORAGE_SIZE,
               "SERDE_WRITER_STORAGE_SIZE is too small for CBOR");

static bool read_argument(const uint8_t* data, size_t size, size_t offset,
                          uint8_t additional, uint64_t* value,
                          size_t* header_size) {
    if (value == NULL || header_size == NULL || offset >= size) return false;
    if (additional < 24U) {
        *value = additional;
        *header_size = 1U;
        return true;
    }
    size_t count = additional == 24U ? 1U : additional == 25U ? 2U
                                      : additional == 26U ? 4U
                                      : additional == 27U ? 8U : 0U;
    if (count == 0U || count > size - offset - 1U) return false;
    uint64_t result = 0U;
    for (size_t index = 0U; index < count; ++index)
        result = (result << 8U) | data[offset + 1U + index];
    *value = result;
    *header_size = 1U + count;
    return true;
}

static bool item_end(const uint8_t* data, size_t size, size_t offset,
                     unsigned int depth, size_t* end) {
    if (data == NULL || end == NULL || offset >= size || depth > CBOR_MAX_DEPTH)
        return false;
    uint8_t initial = data[offset];
    uint8_t major = initial >> 5U;
    uint8_t additional = initial & 31U;
    if (initial == 0xFFU) return false;
    uint64_t argument = 0U;
    size_t header = 0U;
    if (additional == 31U) {
        if (major != 4U && major != 5U) return false;
        size_t cursor = offset + 1U;
        bool map_value = false;
        while (cursor < size && data[cursor] != 0xFFU) {
            if (!item_end(data, size, cursor, depth + 1U, &cursor)) return false;
            if (major == 5U) map_value = !map_value;
        }
        if (cursor >= size || (major == 5U && map_value)) return false;
        *end = cursor + 1U;
        return true;
    }
    if (!read_argument(data, size, offset, additional, &argument, &header))
        return false;
    if (major <= 1U || major == 7U) {
        if (major == 7U && additional >= 28U) return false;
        *end = offset + header;
        return true;
    }
    if (major == 2U || major == 3U) {
        if (argument > SIZE_MAX || (size_t)argument > size - offset - header)
            return false;
        *end = offset + header + (size_t)argument;
        return true;
    }
    if (major == 4U || major == 5U) {
        uint64_t items = major == 5U ? argument * 2U : argument;
        if (major == 5U && argument > UINT64_MAX / 2U) return false;
        size_t cursor = offset + header;
        for (uint64_t index = 0U; index < items; ++index)
            if (!item_end(data, size, cursor, depth + 1U, &cursor)) return false;
        *end = cursor;
        return true;
    }
    return false;
}

static serde_status_t cbor_decode(void* self, const uint8_t* data, size_t size,
                                  serde_node_t* root) {
    serde_cbor_codec_t* cbor = self;
    if (cbor == NULL || !cbor->initialized || data == NULL || size == 0U ||
        size > UINT32_MAX || root == NULL)
        return SERDE_ERROR_STATE;
    size_t end = 0U;
    if (!item_end(data, size, 0U, 0U, &end) || end != size)
        return SERDE_ERROR_MALFORMED;
    cbor->input = data;
    cbor->input_size = size;
    root->value = 1U;
    return SERDE_OK;
}

static bool node_offset(const serde_cbor_codec_t* cbor, serde_node_t node,
                        size_t* offset) {
    if (cbor == NULL || node.value == 0U ||
        (size_t)(node.value - 1U) >= cbor->input_size)
        return false;
    *offset = node.value - 1U;
    return true;
}

static serde_kind_t cbor_kind(const void* self, serde_node_t node) {
    const serde_cbor_codec_t* cbor = self;
    size_t offset = 0U;
    if (!node_offset(cbor, node, &offset)) return SERDE_KIND_INVALID;
    uint8_t initial = cbor->input[offset];
    switch (initial >> 5U) {
        case 0U:
        case 1U: return SERDE_KIND_NUMBER;
        case 3U: return SERDE_KIND_STRING;
        case 4U: return SERDE_KIND_ARRAY;
        case 5U: return SERDE_KIND_OBJECT;
        case 7U:
            if (initial == 0xF4U || initial == 0xF5U) return SERDE_KIND_BOOL;
            if (initial == 0xF6U) return SERDE_KIND_NULL;
            if (initial == 0xF9U || initial == 0xFAU || initial == 0xFBU)
                return SERDE_KIND_NUMBER;
            return SERDE_KIND_INVALID;
        default: return SERDE_KIND_INVALID;
    }
}

static bool container_info(const serde_cbor_codec_t* cbor, serde_node_t node,
                           uint8_t expected_major, size_t* cursor,
                           uint64_t* count, bool* indefinite) {
    size_t offset = 0U;
    if (!node_offset(cbor, node, &offset)) return false;
    uint8_t initial = cbor->input[offset];
    if ((initial >> 5U) != expected_major) return false;
    uint8_t additional = initial & 31U;
    if (additional == 31U) {
        *cursor = offset + 1U;
        *count = CBOR_INDEFINITE;
        *indefinite = true;
        return true;
    }
    size_t header = 0U;
    if (!read_argument(cbor->input, cbor->input_size, offset, additional,
                       count, &header))
        return false;
    *cursor = offset + header;
    *indefinite = false;
    return true;
}

static size_t cbor_container_size(const serde_cbor_codec_t* cbor,
                                  serde_node_t node, uint8_t major) {
    size_t cursor = 0U;
    uint64_t declared = 0U;
    bool indefinite = false;
    if (!container_info(cbor, node, major, &cursor, &declared, &indefinite))
        return 0U;
    if (!indefinite) return declared <= SIZE_MAX ? (size_t)declared : 0U;
    size_t count = 0U;
    while (cursor < cbor->input_size && cbor->input[cursor] != 0xFFU) {
        if (!item_end(cbor->input, cbor->input_size, cursor, 0U, &cursor))
            return 0U;
        if (major == 5U &&
            !item_end(cbor->input, cbor->input_size, cursor, 0U, &cursor))
            return 0U;
        ++count;
    }
    return count;
}

static size_t cbor_object_size(const void* self, serde_node_t object) {
    return cbor_container_size(self, object, 5U);
}

static size_t cbor_array_size(const void* self, serde_node_t array) {
    return cbor_container_size(self, array, 4U);
}

static bool cbor_object_member(const void* self, serde_node_t object,
                               size_t index, serde_key_view_t* key,
                               serde_node_t* value) {
    const serde_cbor_codec_t* cbor = self;
    size_t cursor = 0U;
    uint64_t count = 0U;
    bool indefinite = false;
    if (key == NULL || value == NULL ||
        !container_info(cbor, object, 5U, &cursor, &count, &indefinite) ||
        (!indefinite && index >= count))
        return false;
    for (size_t member = 0U; member < index; ++member) {
        if (!item_end(cbor->input, cbor->input_size, cursor, 0U, &cursor) ||
            !item_end(cbor->input, cbor->input_size, cursor, 0U, &cursor))
            return false;
    }
    if (cursor >= cbor->input_size || cbor->input[cursor] == 0xFFU)
        return false;
    *key = (serde_key_view_t){0};
    uint8_t initial = cbor->input[cursor];
    uint8_t major = initial >> 5U;
    uint64_t argument = 0U;
    size_t header = 0U;
    if (!read_argument(cbor->input, cbor->input_size, cursor, initial & 31U,
                       &argument, &header))
        return false;
    if (major == 0U && argument <= UINT16_MAX && argument != 0U) {
        key->tag = (uint16_t)argument;
        key->has_tag = true;
    } else if (major == 3U && argument <= SIZE_MAX &&
               (size_t)argument <= cbor->input_size - cursor - header) {
        key->name = (const char*)cbor->input + cursor + header;
        key->name_length = (size_t)argument;
        key->has_name = true;
    } else {
        return false;
    }
    if (!item_end(cbor->input, cbor->input_size, cursor, 0U, &cursor) ||
        cursor >= cbor->input_size)
        return false;
    value->value = (uint32_t)cursor + 1U;
    return true;
}

static bool cbor_array_get(const void* self, serde_node_t array, size_t index,
                           serde_node_t* value) {
    const serde_cbor_codec_t* cbor = self;
    size_t cursor = 0U;
    uint64_t count = 0U;
    bool indefinite = false;
    if (value == NULL ||
        !container_info(cbor, array, 4U, &cursor, &count, &indefinite) ||
        (!indefinite && index >= count))
        return false;
    for (size_t item = 0U; item < index; ++item)
        if (!item_end(cbor->input, cbor->input_size, cursor, 0U, &cursor))
            return false;
    if (cursor >= cbor->input_size || cbor->input[cursor] == 0xFFU)
        return false;
    value->value = (uint32_t)cursor + 1U;
    return true;
}

static bool integer_value(const serde_cbor_codec_t* cbor, serde_node_t node,
                          bool* negative, uint64_t* magnitude) {
    size_t offset = 0U;
    if (!node_offset(cbor, node, &offset)) return false;
    uint8_t initial = cbor->input[offset];
    uint8_t major = initial >> 5U;
    size_t header = 0U;
    if (major > 1U || !read_argument(cbor->input, cbor->input_size, offset,
                                    initial & 31U, magnitude, &header))
        return false;
    *negative = major == 1U;
    return true;
}

static bool cbor_get_i32(const void* self, serde_node_t node, int32_t* value) {
    bool negative = false;
    uint64_t magnitude = 0U;
    if (value == NULL || !integer_value(self, node, &negative, &magnitude))
        return false;
    if (!negative && magnitude <= INT32_MAX) {
        *value = (int32_t)magnitude;
        return true;
    }
    if (negative && magnitude <= INT32_MAX) {
        *value = (int32_t)(-1 - (int64_t)magnitude);
        return true;
    }
    return false;
}

static bool cbor_get_u32(const void* self, serde_node_t node, uint32_t* value) {
    bool negative = false;
    uint64_t magnitude = 0U;
    if (value == NULL || !integer_value(self, node, &negative, &magnitude) ||
        negative || magnitude > UINT32_MAX)
        return false;
    *value = (uint32_t)magnitude;
    return true;
}

static float half_to_float(uint16_t half) {
    uint32_t sign = (uint32_t)(half & 0x8000U) << 16U;
    uint32_t exponent = (half >> 10U) & 31U;
    uint32_t fraction = half & 0x03FFU;
    uint32_t bits;
    if (exponent == 0U) {
        if (fraction == 0U) bits = sign;
        else {
            exponent = 113U;
            while ((fraction & 0x0400U) == 0U) {
                fraction <<= 1U;
                --exponent;
            }
            bits = sign | (exponent << 23U) | ((fraction & 0x03FFU) << 13U);
        }
    } else if (exponent == 31U) {
        bits = sign | 0x7F800000U | (fraction << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (fraction << 13U);
    }
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static bool cbor_get_f32(const void* self, serde_node_t node, float* value) {
    const serde_cbor_codec_t* cbor = self;
    int32_t signed_value = 0;
    uint32_t unsigned_value = 0U;
    if (value == NULL) return false;
    if (cbor_get_i32(self, node, &signed_value)) {
        *value = (float)signed_value;
        return true;
    }
    if (cbor_get_u32(self, node, &unsigned_value)) {
        *value = (float)unsigned_value;
        return true;
    }
    size_t offset = 0U;
    if (!node_offset(cbor, node, &offset)) return false;
    uint8_t initial = cbor->input[offset];
    if (initial == 0xF9U && offset + 3U <= cbor->input_size) {
        uint16_t half = ((uint16_t)cbor->input[offset + 1U] << 8U) |
                        cbor->input[offset + 2U];
        *value = half_to_float(half);
    } else if (initial == 0xFAU && offset + 5U <= cbor->input_size) {
        uint32_t bits = 0U;
        for (unsigned int index = 0U; index < 4U; ++index)
            bits = (bits << 8U) | cbor->input[offset + 1U + index];
        memcpy(value, &bits, sizeof(bits));
    } else if (initial == 0xFBU && offset + 9U <= cbor->input_size) {
        uint64_t bits = 0U;
        for (unsigned int index = 0U; index < 8U; ++index)
            bits = (bits << 8U) | cbor->input[offset + 1U + index];
        double wide;
        memcpy(&wide, &bits, sizeof(wide));
        *value = (float)wide;
    } else {
        return false;
    }
    return isfinite(*value);
}

static bool cbor_get_bool(const void* self, serde_node_t node, bool* value) {
    const serde_cbor_codec_t* cbor = self;
    size_t offset = 0U;
    if (value == NULL || !node_offset(cbor, node, &offset) ||
        (cbor->input[offset] != 0xF4U && cbor->input[offset] != 0xF5U))
        return false;
    *value = cbor->input[offset] == 0xF5U;
    return true;
}

static bool cbor_get_string(const void* self, serde_node_t node, char* output,
                            size_t capacity, size_t* length) {
    const serde_cbor_codec_t* cbor = self;
    size_t offset = 0U;
    if (!node_offset(cbor, node, &offset)) return false;
    uint8_t initial = cbor->input[offset];
    uint64_t count = 0U;
    size_t header = 0U;
    if ((initial >> 5U) != 3U || (initial & 31U) == 31U ||
        !read_argument(cbor->input, cbor->input_size, offset, initial & 31U,
                       &count, &header) || count > SIZE_MAX ||
        (size_t)count > cbor->input_size - offset - header)
        return false;
    if (length != NULL) *length = (size_t)count;
    if (output == NULL) return capacity == 0U;
    if ((size_t)count + 1U > capacity) return false;
    memcpy(output, cbor->input + offset + header, (size_t)count);
    output[count] = '\0';
    return true;
}

static bool append_byte(cbor_writer_impl_t* writer, uint8_t value) {
    if (!writer->valid || writer->size >= writer->capacity) {
        writer->valid = false;
        return false;
    }
    writer->output[writer->size++] = value;
    return true;
}

static bool append_uint(cbor_writer_impl_t* writer, uint8_t major,
                        uint64_t value) {
    if (value < 24U) return append_byte(writer, (uint8_t)(major | value));
    size_t count = value <= UINT8_MAX ? 1U : value <= UINT16_MAX ? 2U
                                      : value <= UINT32_MAX ? 4U : 8U;
    uint8_t additional = count == 1U ? 24U : count == 2U ? 25U
                                      : count == 4U ? 26U : 27U;
    if (!append_byte(writer, major | additional)) return false;
    for (size_t index = count; index > 0U; --index)
        if (!append_byte(writer, (uint8_t)(value >> ((index - 1U) * 8U))))
            return false;
    return true;
}

static bool before_value(cbor_writer_impl_t* writer, const serde_key_t* key) {
    if (!writer->valid) return false;
    if (writer->depth == 0U) {
        if (writer->root_written || key != NULL) return false;
        writer->root_written = true;
        return true;
    }
    if (writer->levels[writer->depth - 1U] == 5U) {
        if (key == NULL) return false;
        if (key->tag != 0U) return append_uint(writer, 0U, key->tag);
        if (key->name == NULL) return false;
        size_t length = strlen(key->name);
        if (!append_uint(writer, 0x60U, length)) return false;
        for (size_t index = 0U; index < length; ++index)
            if (!append_byte(writer, (uint8_t)key->name[index])) return false;
        return true;
    }
    return key == NULL;
}

static bool begin_container(void* self, const serde_key_t* key, uint8_t major) {
    cbor_writer_impl_t* writer = self;
    if (writer->depth >= CBOR_MAX_DEPTH || !before_value(writer, key) ||
        !append_byte(writer, major == 5U ? 0xBFU : 0x9FU))
        return false;
    writer->levels[writer->depth++] = major;
    return true;
}

static bool cbor_begin_object(void* self, const serde_key_t* key) {
    return begin_container(self, key, 5U);
}
static bool cbor_begin_array(void* self, const serde_key_t* key) {
    return begin_container(self, key, 4U);
}
static bool end_container(void* self, uint8_t major) {
    cbor_writer_impl_t* writer = self;
    if (!writer->valid || writer->depth == 0U ||
        writer->levels[writer->depth - 1U] != major)
        return false;
    if (!append_byte(writer, 0xFFU)) return false;
    --writer->depth;
    return true;
}
static bool cbor_end_object(void* self) { return end_container(self, 5U); }
static bool cbor_end_array(void* self) { return end_container(self, 4U); }

static bool cbor_put_i32(void* self, const serde_key_t* key, int32_t value) {
    cbor_writer_impl_t* writer = self;
    if (!before_value(writer, key)) return false;
    return value >= 0 ? append_uint(writer, 0U, (uint32_t)value)
                      : append_uint(writer, 0x20U,
                                    (uint32_t)(-(int64_t)value - 1));
}
static bool cbor_put_u32(void* self, const serde_key_t* key, uint32_t value) {
    cbor_writer_impl_t* writer = self;
    return before_value(writer, key) && append_uint(writer, 0U, value);
}
static bool cbor_put_f32(void* self, const serde_key_t* key, float value) {
    cbor_writer_impl_t* writer = self;
    uint32_t bits = 0U;
    if (!isfinite(value) || !before_value(writer, key) ||
        !append_byte(writer, 0xFAU))
        return false;
    memcpy(&bits, &value, sizeof(bits));
    for (unsigned int shift = 32U; shift > 0U; shift -= 8U)
        if (!append_byte(writer, (uint8_t)(bits >> (shift - 8U)))) return false;
    return true;
}
static bool cbor_put_bool(void* self, const serde_key_t* key, bool value) {
    cbor_writer_impl_t* writer = self;
    return before_value(writer, key) &&
           append_byte(writer, value ? 0xF5U : 0xF4U);
}
static bool cbor_put_string(void* self, const serde_key_t* key,
                            const char* value, size_t length) {
    cbor_writer_impl_t* writer = self;
    if ((value == NULL && length != 0U) || !before_value(writer, key) ||
        !append_uint(writer, 0x60U, length))
        return false;
    for (size_t index = 0U; index < length; ++index)
        if (!append_byte(writer, (uint8_t)value[index])) return false;
    return true;
}
static bool cbor_put_null(void* self, const serde_key_t* key) {
    cbor_writer_impl_t* writer = self;
    return before_value(writer, key) && append_byte(writer, 0xF6U);
}
static bool cbor_finish(void* self, size_t* encoded_size) {
    cbor_writer_impl_t* writer = self;
    if (!writer->valid || writer->depth != 0U || !writer->root_written ||
        encoded_size == NULL)
        return false;
    *encoded_size = writer->size;
    return true;
}
static bool cbor_writer_ok(const void* self) {
    const cbor_writer_impl_t* writer = self;
    return writer != NULL && writer->valid;
}
static bool cbor_writer_open(void* self, serde_writer_storage_t* storage,
                             uint8_t* output, size_t capacity,
                             serde_writer_t* writer) {
    serde_cbor_codec_t* cbor = self;
    if (cbor == NULL || !cbor->initialized || storage == NULL ||
        output == NULL || capacity == 0U || writer == NULL)
        return false;
    cbor_writer_impl_t* impl = (void*)storage->bytes;
    *impl = (cbor_writer_impl_t){.output = output,
                                 .capacity = capacity,
                                 .valid = true};
    static const serde_writer_mtab_t mtab = {
        .begin_object = cbor_begin_object,
        .end_object = cbor_end_object,
        .begin_array = cbor_begin_array,
        .end_array = cbor_end_array,
        .put_i32 = cbor_put_i32,
        .put_u32 = cbor_put_u32,
        .put_f32 = cbor_put_f32,
        .put_bool = cbor_put_bool,
        .put_string = cbor_put_string,
        .put_null = cbor_put_null,
        .finish = cbor_finish,
        .ok = cbor_writer_ok};
    *writer = (serde_writer_t){.mtab = &mtab, .self = impl};
    return true;
}

static const serde_codec_mtab_t cbor_codec_mtab = {
    .decode = cbor_decode,
    .kind = cbor_kind,
    .object_size = cbor_object_size,
    .object_member = cbor_object_member,
    .array_size = cbor_array_size,
    .array_get = cbor_array_get,
    .get_i32 = cbor_get_i32,
    .get_u32 = cbor_get_u32,
    .get_f32 = cbor_get_f32,
    .get_bool = cbor_get_bool,
    .get_string = cbor_get_string,
    .writer_open = cbor_writer_open};

bool serde_cbor_init(serde_cbor_codec_t* cbor) {
    if (cbor == NULL) return false;
    *cbor = (serde_cbor_codec_t){.initialized = true};
    return true;
}

serde_codec_t serde_cbor(serde_cbor_codec_t* cbor) {
    if (cbor == NULL) return (serde_codec_t){0};
    return (serde_codec_t){.mtab = &cbor_codec_mtab, .self = cbor};
}
