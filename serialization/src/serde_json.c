#define SERDE_JSON_IMPLEMENTATION
#define JSMN_STRICT
#include "serde_json.h"

/*
 * Кодек JSON для serde без динамической памяти.
 *
 * Чтение: jsmn размечает исходный буфер токенами. serde_node_t.value — индекс
 * токена, поэтому обход объектов/массивов не копирует данные.
 *
 * Запись: json_writer_impl_t последовательно добавляет байты в буфер клиента и
 * хранит небольшой стек открытых object/array. При первой ошибке failed
 * фиксируется до конца работы, чтобы частичный JSON нельзя было принять за
 * корректный результат.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Независимый предел JSON writer; KRUL дополнительно ограничивает свою глубину. */
#define JSON_MAX_DEPTH 12U

typedef struct {
    /* kind равен '{' или '[', first управляет автоматической запятой. */
    char kind;
    bool first;
} json_level_t;

typedef struct {
    /* Полностью помещается в serde_writer_storage_t, heap не используется. */
    uint8_t* buffer;
    size_t capacity;
    size_t length;
    bool failed;
    uint8_t depth;
    json_level_t levels[JSON_MAX_DEPTH];
} json_writer_impl_t;

_Static_assert(sizeof(json_writer_impl_t) <= SERDE_WRITER_STORAGE_SIZE,
               "SERDE_WRITER_STORAGE_SIZE is too small for JSON");

static bool valid_node(const serde_json_codec_t* json, serde_node_t node) {
    return json != NULL && node.value < (uint32_t)json->token_count;
}

static const jsmntok_t* token_at(const serde_json_codec_t* json,
                                 serde_node_t node) {
    return valid_node(json, node)
               ? &json->tokens[node.value]
               : NULL;
}

static serde_status_t json_decode(void* self, const uint8_t* data, size_t size,
                                  serde_node_t* root) {
    serde_json_codec_t* json = self;
    if (json == NULL || data == NULL || root == NULL || json->tokens == NULL ||
        json->token_capacity == 0U)
        return SERDE_ERROR_STATE;

    /* Токены ссылаются на data, поэтому сохраняем input только после полной
     * успешной проверки кадра. */
    jsmn_parser parser;
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, (const char*)data, size, json->tokens,
                           (unsigned int)json->token_capacity);
    if (count < 1) {
        json->token_count = 0;
        return count == JSMN_ERROR_NOMEM ? SERDE_ERROR_NO_SPACE
                                         : SERDE_ERROR_MALFORMED;
    }
    /* jsmn возвращает первый JSON value; запрещаем непустой хвост после него. */
    const jsmntok_t* tokens = json->tokens;
    for (size_t index = (size_t)tokens[0].end; index < size; ++index) {
        if (!isspace(data[index])) {
            json->token_count = 0;
            return SERDE_ERROR_MALFORMED;
        }
    }
    json->input = data;
    json->input_size = size;
    json->token_count = count;
    *root = (serde_node_t){.value = 0U};
    return SERDE_OK;
}

static serde_kind_t json_kind(const void* self, serde_node_t node) {
    const serde_json_codec_t* json = self;
    const jsmntok_t* token = token_at(json, node);
    if (token == NULL) return SERDE_KIND_INVALID;
    switch (token->type) {
        case JSMN_OBJECT:
            return SERDE_KIND_OBJECT;
        case JSMN_ARRAY:
            return SERDE_KIND_ARRAY;
        case JSMN_STRING:
            return SERDE_KIND_STRING;
        case JSMN_PRIMITIVE: {
            size_t length = (size_t)(token->end - token->start);
            const char* value = (const char*)json->input + token->start;
            if ((length == 4U && memcmp(value, "true", 4U) == 0) ||
                (length == 5U && memcmp(value, "false", 5U) == 0))
                return SERDE_KIND_BOOL;
            if (length == 4U && memcmp(value, "null", 4U) == 0)
                return SERDE_KIND_NULL;
            return SERDE_KIND_NUMBER;
        }
        default:
            return SERDE_KIND_INVALID;
    }
}

static size_t json_object_size(const void* self, serde_node_t object) {
    const serde_json_codec_t* json = self;
    const jsmntok_t* token = token_at(json, object);
    if (token == NULL || token->type != JSMN_OBJECT) return 0U;
    size_t count = 0U;
    const jsmntok_t* tokens = json->tokens;
    for (int index = (int)object.value + 1; index < json->token_count; ++index) {
        if (tokens[index].start >= token->end) break;
        if (tokens[index].parent == (int)object.value &&
            tokens[index].type == JSMN_STRING)
            ++count;
    }
    return count;
}

static bool json_object_member(const void* self, serde_node_t object,
                               size_t wanted, serde_key_view_t* key,
                               serde_node_t* value) {
    const serde_json_codec_t* json = self;
    const jsmntok_t* object_token = token_at(json, object);
    if (object_token == NULL || object_token->type != JSMN_OBJECT ||
        key == NULL || value == NULL)
        return false;
    /* В jsmn значение поля является следующим токеном и ссылается parent на
     * токен имени ключа. Вложенные потомки пропускаются по parent. */
    const jsmntok_t* tokens = json->tokens;
    size_t found = 0U;
    for (int index = (int)object.value + 1; index < json->token_count; ++index) {
        if (tokens[index].start >= object_token->end) break;
        if (tokens[index].parent == (int)object.value &&
            tokens[index].type == JSMN_STRING) {
            if (found++ != wanted) continue;
            if (index + 1 >= json->token_count ||
                tokens[index + 1].parent != index)
                return false;
            *key = (serde_key_view_t){
                .name = (const char*)json->input + tokens[index].start,
                .name_length = (size_t)(tokens[index].end - tokens[index].start),
                .has_name = true};
            *value = (serde_node_t){.value = (uint32_t)(index + 1)};
            return true;
        }
    }
    return false;
}

static size_t json_array_size(const void* self, serde_node_t array) {
    const serde_json_codec_t* json = self;
    const jsmntok_t* token = token_at(json, array);
    if (token == NULL || token->type != JSMN_ARRAY) return 0U;
    size_t count = 0U;
    const jsmntok_t* tokens = json->tokens;
    for (int index = (int)array.value + 1; index < json->token_count; ++index) {
        if (tokens[index].start >= token->end) break;
        if (tokens[index].parent == (int)array.value) ++count;
    }
    return count;
}

static bool json_array_get(const void* self, serde_node_t array, size_t wanted,
                           serde_node_t* value) {
    const serde_json_codec_t* json = self;
    const jsmntok_t* array_token = token_at(json, array);
    if (array_token == NULL || array_token->type != JSMN_ARRAY || value == NULL)
        return false;
    const jsmntok_t* tokens = json->tokens;
    size_t found = 0U;
    for (int index = (int)array.value + 1; index < json->token_count; ++index) {
        if (tokens[index].start >= array_token->end) break;
        if (tokens[index].parent == (int)array.value) {
            if (found++ == wanted) {
                *value = (serde_node_t){.value = (uint32_t)index};
                return true;
            }
        }
    }
    return false;
}

static bool token_number(const serde_json_codec_t* json,
                         const jsmntok_t* token, char* output,
                         size_t capacity) {
    if (token == NULL || token->type != JSMN_PRIMITIVE || token->start < 0 ||
        token->end <= token->start)
        return false;
    /* Сначала строго проверяем JSON-грамматику числа. strto* самостоятельно
     * принимает формы, которые протокол JSON принимать не должен. */
    size_t length = (size_t)(token->end - token->start);
    if (length + 1U > capacity) return false;
    memcpy(output, json->input + token->start, length);
    output[length] = '\0';
    size_t index = 0U;
    if (output[index] == '-' && ++index == length) return false;
    if (output[index] == '0') {
        ++index;
    } else {
        if (output[index] < '1' || output[index] > '9') return false;
        while (++index < length && output[index] >= '0' &&
               output[index] <= '9') {
        }
    }
    if (index < length && output[index] == '.') {
        if (++index == length || output[index] < '0' || output[index] > '9')
            return false;
        while (++index < length && output[index] >= '0' &&
               output[index] <= '9') {
        }
    }
    if (index < length && (output[index] == 'e' || output[index] == 'E')) {
        ++index;
        if (index < length && (output[index] == '+' || output[index] == '-'))
            ++index;
        if (index == length || output[index] < '0' || output[index] > '9')
            return false;
        while (++index < length && output[index] >= '0' &&
               output[index] <= '9') {
        }
    }
    if (index != length) return false;
    return true;
}

static bool json_get_i32(const void* self, serde_node_t node, int32_t* output) {
    const serde_json_codec_t* json = self;
    char text[24];
    if (output == NULL || !token_number(json, token_at(json, node), text,
                                        sizeof(text)))
        return false;
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < INT32_MIN ||
        value > INT32_MAX)
        return false;
    *output = (int32_t)value;
    return true;
}

static bool json_get_u32(const void* self, serde_node_t node,
                         uint32_t* output) {
    const serde_json_codec_t* json = self;
    char text[24];
    if (output == NULL || !token_number(json, token_at(json, node), text,
                                        sizeof(text)) ||
        text[0] == '-')
        return false;
    char* end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX)
        return false;
    *output = (uint32_t)value;
    return true;
}

static bool json_get_f32(const void* self, serde_node_t node, float* output) {
    const serde_json_codec_t* json = self;
    char text[48];
    if (output == NULL || !token_number(json, token_at(json, node), text,
                                        sizeof(text)))
        return false;
    char* end = NULL;
    errno = 0;
    float value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value))
        return false;
    *output = value;
    return true;
}

static bool json_get_bool(const void* self, serde_node_t node, bool* output) {
    const serde_json_codec_t* json = self;
    const jsmntok_t* token = token_at(json, node);
    if (output == NULL || token == NULL || token->type != JSMN_PRIMITIVE)
        return false;
    size_t length = (size_t)(token->end - token->start);
    const uint8_t* value = json->input + token->start;
    if (length == 4U && memcmp(value, "true", 4U) == 0) {
        *output = true;
        return true;
    }
    if (length == 5U && memcmp(value, "false", 5U) == 0) {
        *output = false;
        return true;
    }
    return false;
}

static int hex_value(uint8_t value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool append_utf8(uint32_t codepoint, char* output, size_t capacity,
                        size_t* offset) {
    /* Преобразование \uXXXX в UTF-8. Пара суррогатов собирается уровнем выше. */
    uint8_t bytes[4];
    size_t count = 0U;
    if (codepoint <= 0x7FU) {
        bytes[count++] = (uint8_t)codepoint;
    } else if (codepoint <= 0x7FFU) {
        bytes[count++] = (uint8_t)(0xC0U | (codepoint >> 6U));
        bytes[count++] = (uint8_t)(0x80U | (codepoint & 0x3FU));
    } else if (codepoint <= 0xFFFFU) {
        bytes[count++] = (uint8_t)(0xE0U | (codepoint >> 12U));
        bytes[count++] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[count++] = (uint8_t)(0x80U | (codepoint & 0x3FU));
    } else if (codepoint <= 0x10FFFFU) {
        bytes[count++] = (uint8_t)(0xF0U | (codepoint >> 18U));
        bytes[count++] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3FU));
        bytes[count++] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[count++] = (uint8_t)(0x80U | (codepoint & 0x3FU));
    } else {
        return false;
    }
    if (output != NULL) {
        if (*offset + count >= capacity) return false;
        memcpy(output + *offset, bytes, count);
    }
    *offset += count;
    return true;
}

static bool json_get_string(const void* self, serde_node_t node, char* output,
                            size_t capacity, size_t* decoded_length) {
    const serde_json_codec_t* json = self;
    const jsmntok_t* token = token_at(json, node);
    if (token == NULL || token->type != JSMN_STRING ||
        (output != NULL && capacity == 0U))
        return false;
    /* Декодируем escape-последовательности, а не просто копируем срез токена. */
    size_t offset = 0U;
    for (int index = token->start; index < token->end; ++index) {
        uint8_t value = json->input[index];
        if (value != '\\') {
            if (output != NULL) {
                if (offset + 1U >= capacity) return false;
                output[offset] = (char)value;
            }
            ++offset;
            continue;
        }
        if (++index >= token->end) return false;
        uint8_t escaped = json->input[index];
        char decoded = '\0';
        switch (escaped) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u': {
                if (index + 4 >= token->end) return false;
                uint32_t codepoint = 0U;
                for (int digit = 0; digit < 4; ++digit) {
                    int hex = hex_value(json->input[++index]);
                    if (hex < 0) return false;
                    codepoint = (codepoint << 4U) | (uint32_t)hex;
                }
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (index + 6 >= token->end ||
                        json->input[index + 1] != '\\' ||
                        json->input[index + 2] != 'u')
                        return false;
                    index += 2;
                    uint32_t low = 0U;
                    for (int digit = 0; digit < 4; ++digit) {
                        int hex = hex_value(json->input[++index]);
                        if (hex < 0) return false;
                        low = (low << 4U) | (uint32_t)hex;
                    }
                    if (low < 0xDC00U || low > 0xDFFFU) return false;
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                                (low - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    return false;
                }
                if (codepoint == 0U ||
                    !append_utf8(codepoint, output, capacity, &offset))
                    return false;
                continue;
            }
            default: return false;
        }
        if (output != NULL) {
            if (offset + 1U >= capacity) return false;
            output[offset] = decoded;
        }
        ++offset;
    }
    if (output != NULL) output[offset] = '\0';
    if (decoded_length != NULL) *decoded_length = offset;
    return true;
}

static bool append_bytes(json_writer_impl_t* writer, const void* data,
                         size_t length) {
    /* failed «прилипает»: после переполнения последующие операции тоже ложны. */
    if (writer->failed) return false;
    if (length > writer->capacity - writer->length) {
        writer->failed = true;
        return false;
    }
    memcpy(writer->buffer + writer->length, data, length);
    writer->length += length;
    return true;
}

static bool append_char(json_writer_impl_t* writer, char value) {
    return append_bytes(writer, &value, 1U);
}

static bool append_escaped(json_writer_impl_t* writer, const char* value,
                           size_t length) {
    /* Кавычки, slash и управляющие байты экранируются по правилам JSON. */
    if (!append_char(writer, '"')) return false;
    for (size_t index = 0U; index < length; ++index) {
        const char* escaped = NULL;
        switch ((uint8_t)value[index]) {
            case '"': escaped = "\\\""; break;
            case '\\': escaped = "\\\\"; break;
            case '\b': escaped = "\\b"; break;
            case '\f': escaped = "\\f"; break;
            case '\n': escaped = "\\n"; break;
            case '\r': escaped = "\\r"; break;
            case '\t': escaped = "\\t"; break;
            default: break;
        }
        if (escaped != NULL) {
            if (!append_bytes(writer, escaped, 2U)) return false;
        } else if ((uint8_t)value[index] < 0x20U) {
            char unicode[7];
            int count = snprintf(unicode, sizeof(unicode), "\\u%04x",
                                 (unsigned int)(uint8_t)value[index]);
            if (count != 6 || !append_bytes(writer, unicode, 6U)) return false;
        } else if (!append_char(writer, value[index])) {
            return false;
        }
    }
    return append_char(writer, '"');
}

static bool before_value(json_writer_impl_t* writer, const serde_key_t* key) {
    if (writer->failed) return false;
    /* Корневое значение не имеет ключа и может быть записано только один раз. */
    if (writer->depth == 0U) return key == NULL && writer->length == 0U;
    json_level_t* level = &writer->levels[writer->depth - 1U];
    if (!level->first && !append_char(writer, ',')) return false;
    level->first = false;
    /* В object ключ обязателен, в array — запрещён. */
    if (level->kind == '{') {
        if (key == NULL || key->name == NULL) {
            writer->failed = true;
            return false;
        }
        if (!append_escaped(writer, key->name, strlen(key->name)) ||
            !append_char(writer, ':'))
            return false;
    } else if (key != NULL) {
        writer->failed = true;
        return false;
    }
    return true;
}

static bool json_begin_container(void* self, const serde_key_t* key, char kind) {
    json_writer_impl_t* writer = self;
    if (writer->depth >= JSON_MAX_DEPTH || !before_value(writer, key) ||
        !append_char(writer, kind)) {
        writer->failed = true;
        return false;
    }
    writer->levels[writer->depth++] =
        (json_level_t){.kind = kind, .first = true};
    return true;
}

static bool json_begin_object(void* self, const serde_key_t* key) {
    return json_begin_container(self, key, '{');
}

static bool json_begin_array(void* self, const serde_key_t* key) {
    return json_begin_container(self, key, '[');
}

static bool json_end_container(void* self, char kind, char closing) {
    json_writer_impl_t* writer = self;
    if (writer->failed || writer->depth == 0U ||
        writer->levels[writer->depth - 1U].kind != kind) {
        writer->failed = true;
        return false;
    }
    --writer->depth;
    return append_char(writer, closing);
}

static bool json_end_object(void* self) {
    return json_end_container(self, '{', '}');
}

static bool json_end_array(void* self) {
    return json_end_container(self, '[', ']');
}

static bool json_put_formatted(json_writer_impl_t* writer,
                               const serde_key_t* key, const char* format,
                               ...) {
    if (!before_value(writer, key)) return false;
    char buffer[48];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (count <= 0 || (size_t)count >= sizeof(buffer)) {
        writer->failed = true;
        return false;
    }
    return append_bytes(writer, buffer, (size_t)count);
}

static bool json_put_i32(void* self, const serde_key_t* key, int32_t value) {
    return json_put_formatted(self, key, "%ld", (long)value);
}

static bool json_put_u32(void* self, const serde_key_t* key, uint32_t value) {
    return json_put_formatted(self, key, "%lu", (unsigned long)value);
}

static bool json_put_f32(void* self, const serde_key_t* key, float value) {
    json_writer_impl_t* writer = self;
    if (!isfinite(value)) {
        writer->failed = true;
        return false;
    }
    return json_put_formatted(writer, key, "%.9g", (double)value);
}

static bool json_put_bool(void* self, const serde_key_t* key, bool value) {
    json_writer_impl_t* writer = self;
    return before_value(writer, key) &&
           append_bytes(writer, value ? "true" : "false", value ? 4U : 5U);
}

static bool json_put_string(void* self, const serde_key_t* key,
                            const char* value, size_t length) {
    json_writer_impl_t* writer = self;
    return before_value(writer, key) && append_escaped(writer, value, length);
}

static bool json_put_null(void* self, const serde_key_t* key) {
    json_writer_impl_t* writer = self;
    return before_value(writer, key) && append_bytes(writer, "null", 4U);
}

static bool json_finish(void* self, size_t* encoded_size) {
    json_writer_impl_t* writer = self;
    if (writer->failed || writer->depth != 0U || writer->length == 0U ||
        writer->length >= writer->capacity) {
        writer->failed = true;
        return false;
    }
    /* Терминатор удобен для диагностики, но в encoded_size не учитывается. */
    writer->buffer[writer->length] = '\0';
    if (encoded_size != NULL) *encoded_size = writer->length;
    return true;
}

static bool json_ok(const void* self) {
    const json_writer_impl_t* writer = self;
    return writer != NULL && !writer->failed;
}

static const serde_writer_mtab_t json_writer_mtab = {
    /* Таблица связывает общий serde_writer_t с JSON-реализацией выше. */
    .begin_object = json_begin_object,
    .end_object = json_end_object,
    .begin_array = json_begin_array,
    .end_array = json_end_array,
    .put_i32 = json_put_i32,
    .put_u32 = json_put_u32,
    .put_f32 = json_put_f32,
    .put_bool = json_put_bool,
    .put_string = json_put_string,
    .put_null = json_put_null,
    .finish = json_finish,
    .ok = json_ok};

static bool json_writer_open(void* self, serde_writer_storage_t* storage,
                             uint8_t* output, size_t capacity,
                             serde_writer_t* writer) {
    (void)self;
    if (storage == NULL || output == NULL || capacity == 0U || writer == NULL)
        return false;
    json_writer_impl_t* impl = (json_writer_impl_t*)storage->bytes;
    *impl = (json_writer_impl_t){.buffer = output, .capacity = capacity};
    *writer = (serde_writer_t){.mtab = &json_writer_mtab, .self = impl};
    return true;
}

static const serde_codec_mtab_t json_codec_mtab = {
    /* Единственный объект методов; состояние конкретного кодека находится self. */
    .decode = json_decode,
    .kind = json_kind,
    .object_size = json_object_size,
    .object_member = json_object_member,
    .array_size = json_array_size,
    .array_get = json_array_get,
    .get_i32 = json_get_i32,
    .get_u32 = json_get_u32,
    .get_f32 = json_get_f32,
    .get_bool = json_get_bool,
    .get_string = json_get_string,
    .writer_open = json_writer_open};

bool serde_json_init(serde_json_codec_t* json, serde_json_token_t* tokens,
                     size_t token_capacity) {
    if (json == NULL || tokens == NULL || token_capacity == 0U ||
        token_capacity > UINT_MAX)
        return false;
    *json = (serde_json_codec_t){.tokens = tokens,
                                  .token_capacity = token_capacity};
    return true;
}

serde_codec_t serde_json(serde_json_codec_t* json) {
    if (json == NULL) return (serde_codec_t){0};
    return (serde_codec_t){.mtab = &json_codec_mtab, .self = json};
}
