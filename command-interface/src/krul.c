#include "krul_internal.h"

/*
 * Общие операции KRUL, не относящиеся к конкретной фазе диспетчеризации:
 * поиск дескриптора, заполнение ошибки и кодирование самостоятельных конвертов
 * error/event/log. Сами команды и их результат здесь не исполняются.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const krul_field_desc_t* krul_find_field(const krul_field_desc_t* fields,
                                         uint16_t count, const char* name,
                                         uint16_t* found_index) {
    /* Линейный поиск оправдан небольшими статическими таблицами команд. */
    if (fields == NULL || name == NULL) return NULL;
    for (uint16_t index = 0U; index < count; ++index) {
        if (fields[index].name != NULL &&
            strcmp(fields[index].name, name) == 0) {
            if (found_index != NULL) *found_index = index;
            return &fields[index];
        }
    }
    return NULL;
}

const char* krul_field_name(const krul_field_desc_t* field) {
    return field != NULL && field->name != NULL ? field->name : "array item";
}

void krul_error_set(krul_error_t* error, krul_status_t code,
                    const char* message_format, ...) {
    if (error == NULL) return;
    error->code = code;
    if (message_format == NULL) {
        error->message[0] = '\0';
        return;
    }
    va_list args;
    va_start(args, message_format);
    (void)vsnprintf(error->message, sizeof(error->message), message_format,
                    args);
    va_end(args);
}

const krul_command_t* krul_find_command(const krul_server_t* server,
                                        const char* name) {
    if (server == NULL || !server->initialized || name == NULL) return NULL;
    for (size_t index = 0U; index < server->config.command_count; ++index) {
        if (strcmp(server->config.commands[index]->name, name) == 0)
            return server->config.commands[index];
    }
    return NULL;
}

size_t krul_encode_error(krul_server_t* server, uint32_t id,
                         krul_status_t code, const char* message,
                         uint8_t* output, size_t capacity) {
    if (server == NULL || !server->initialized) return 0U;
    /* Ошибка формируется с нуля и может безопасно перезаписать частичный
     * успешный ответ, оставшийся после ошибки обработчика или записи. */
    serde_writer_storage_t storage;
    serde_writer_t writer;
    if (!serde_writer_open(server->config.codec, &storage, output, capacity,
                           &writer))
        return 0U;
    serde_key_t id_key = krul_named_key("id");
    serde_key_t success = krul_named_key("success");
    serde_key_t error = krul_named_key("error");
    serde_key_t code_key = krul_named_key("code");
    serde_key_t message_key = krul_named_key("message");
    serde_begin_object(writer, NULL);
    serde_put_u32(writer, &id_key, id);
    serde_put_bool(writer, &success, false);
    serde_begin_object(writer, &error);
    serde_put_i32(writer, &code_key, (int32_t)code);
    serde_put_string(writer, &message_key, message != NULL ? message : "Error");
    serde_end_object(writer);
    serde_end_object(writer);
    size_t encoded = 0U;
    return serde_writer_finish(writer, &encoded) ? encoded : 0U;
}

size_t krul_encode_event(krul_server_t* server, const char* event,
                         const krul_event_field_t* fields, size_t field_count,
                         uint8_t* output, size_t capacity) {
    if (server == NULL || !server->initialized || event == NULL ||
        (field_count > 0U && fields == NULL))
        return 0U;
    /* Event не содержит transaction id: он не является ответом на команду. */
    serde_writer_storage_t storage;
    serde_writer_t writer;
    if (!serde_writer_open(server->config.codec, &storage, output, capacity,
                           &writer))
        return 0U;
    serde_key_t event_key = krul_named_key("event");
    serde_key_t data_key = krul_named_key("data");
    serde_begin_object(writer, NULL);
    serde_put_string(writer, &event_key, event);
    serde_begin_object(writer, &data_key);
    for (size_t index = 0U; index < field_count; ++index) {
        if (fields[index].name == NULL) return 0U;
        serde_key_t key = krul_named_key(fields[index].name);
        switch (fields[index].type) {
            case KRUL_TYPE_I32:
                serde_put_i32(writer, &key, fields[index].value.i32);
                break;
            case KRUL_TYPE_U32:
                serde_put_u32(writer, &key, fields[index].value.u32);
                break;
            case KRUL_TYPE_F32:
                serde_put_f32(writer, &key, fields[index].value.f32);
                break;
            case KRUL_TYPE_BOOL:
                serde_put_bool(writer, &key, fields[index].value.boolean);
                break;
            case KRUL_TYPE_STRING:
            case KRUL_TYPE_ENUM:
            case KRUL_TYPE_CONSOLE_STRING:
                if (fields[index].value.string == NULL) return 0U;
                serde_put_string(writer, &key, fields[index].value.string);
                break;
            default:
                return 0U;
        }
    }
    serde_end_object(writer);
    serde_end_object(writer);
    size_t encoded = 0U;
    return serde_writer_finish(writer, &encoded) ? encoded : 0U;
}

static const char* log_severity_name(krul_console_type_t severity) {
    switch (severity) {
        case KRUL_CONSOLE_DEBUG: return "debug";
        case KRUL_CONSOLE_WARNING: return "warning";
        case KRUL_CONSOLE_ERROR: return "error";
        default: return "info";
    }
}

size_t krul_encode_log_event(krul_server_t* server,
                             krul_console_type_t severity,
                             const char* message, uint8_t* output,
                             size_t capacity) {
    if (message == NULL) return 0U;
    const krul_event_field_t fields[] = {
        {.name = "severity",
         .type = KRUL_TYPE_STRING,
         .value.string = log_severity_name(severity)},
        {.name = "message",
         .type = KRUL_TYPE_STRING,
         .value.string = message}};
    return krul_encode_event(server, "log", fields, KRUL_ARRAY_SIZE(fields),
                             output, capacity);
}

size_t krul_encode_log_eventf(krul_server_t* server,
                              krul_console_type_t severity,
                              uint8_t* output, size_t capacity,
                              const char* format, ...) {
    if (format == NULL) return 0U;
    /* Отказ вместо молчаливой обрезки позволяет вызывающему коду заметить,
     * что лог не помещается в гарантированный размер. */
    char message[KRUL_MAX_LOG_MESSAGE];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= sizeof(message)) return 0U;
    return krul_encode_log_event(server, severity, message, output, capacity);
}
