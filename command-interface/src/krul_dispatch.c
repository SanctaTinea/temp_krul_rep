#include "krul_internal.h"

#include <stdio.h>
#include <string.h>

#define KRUL_MAX_COMMAND_NAME 48U

static bool unique_object_get(serde_codec_t codec, serde_node_t object,
                              const char* name, serde_node_t* value,
                              krul_error_t* error, bool required,
                              bool* present) {
    serde_key_t key = krul_named_key(name);
    size_t matches = 0U;
    bool found = serde_object_get(codec, object, &key, value, &matches);
    if (present != NULL) *present = found;
    if (matches > 1U) {
        krul_error_set(error, KRUL_ERROR_INVALID_REQUEST,
                       "Duplicate field '%s'", name);
        return false;
    }
    if (!found && required) {
        krul_error_set(error, KRUL_ERROR_MISSING_FIELD,
                       "Missing field '%s'", name);
        return false;
    }
    return found || !required;
}

static bool begin_response(const krul_server_t* server, uint32_t id,
                           serde_writer_storage_t* storage,
                           serde_writer_t* writer, bool with_result) {
    if (!serde_writer_open(server->config.codec, storage,
                           server->config.response_buffer,
                           server->config.response_capacity, writer))
        return false;
    serde_key_t id_key = krul_named_key("id");
    serde_key_t success_key = krul_named_key("success");
    if (!serde_begin_object(*writer, NULL) ||
        !serde_put_u32(*writer, &id_key, id) ||
        !serde_put_bool(*writer, &success_key, true))
        return false;
    if (!with_result) return true;
    serde_key_t result_key = krul_named_key("result");
    return serde_begin_object(*writer, &result_key);
}

static void init_result(krul_result_t* result, serde_writer_t writer,
                        krul_error_t* error, krul_server_t* server,
                        const krul_command_t* command, uint32_t id) {
    *result = (krul_result_t){
        .writer = writer,
        .frames = {{.kind = RESULT_FRAME_OBJECT,
                    .fields = command->result,
                    .field_count = command->result_count}},
        .depth = 1U,
        .error = error,
        .server = server,
        .command = command,
        .transaction_id = id};
}

static bool validate_result(krul_result_t* result, bool success) {
    if (result->deferred != NULL) return success;
    if (success && result->depth != 1U) {
        krul_result_fail(result, "Handler left a result container open");
        success = false;
    }
    if (success && !krul_result_object_complete(&result->frames[0])) {
        krul_result_fail(result, "Handler omitted a required result field");
        success = false;
    }
    return success && krul_result_ok(result);
}

static size_t finish_response(krul_server_t* server, uint32_t id,
                              serde_writer_t writer, krul_error_t* error,
                              bool success, bool with_result) {
    if (!success) {
        if (error->message[0] == '\0')
            krul_error_set(error, KRUL_ERROR_EXECUTION,
                           "Command execution failed");
        return krul_encode_error(server, id, error->code, error->message,
                                 server->config.response_buffer,
                                 server->config.response_capacity);
    }
    if (with_result) serde_end_object(writer);
    serde_end_object(writer);
    size_t encoded = 0U;
    if (!serde_writer_finish(writer, &encoded))
        return krul_encode_error(server, id, KRUL_ERROR_RESPONSE_TOO_LARGE,
                                 "Response exceeds output buffer",
                                 server->config.response_buffer,
                                 server->config.response_capacity);
    return encoded;
}

static krul_dispatch_status_t publish(krul_server_t* server, size_t size) {
    if (size == 0U) return KRUL_DISPATCH_FAILED;
    server->response_size = size;
    server->response_slot = UINT16_MAX;
    server->response_ready = true;
    return KRUL_DISPATCH_RESPONSE_READY;
}

static krul_dispatch_status_t publish_error(krul_server_t* server,
                                             uint32_t id,
                                             krul_status_t code,
                                             const char* message) {
    return publish(server, krul_encode_error(
        server, id, code, message, server->config.response_buffer,
        server->config.response_capacity));
}

static krul_pending_slot_t* find_slot(const krul_server_t* server,
                                      krul_pending_handle_t handle) {
    if (server == NULL || !server->initialized ||
        handle.index >= server->config.pending_slot_count)
        return NULL;
    krul_pending_slot_t* slot = &server->config.pending_slots[handle.index];
    if (slot->state == KRUL_PENDING_FREE ||
        slot->generation != handle.generation)
        return NULL;
    return slot;
}

static void free_slot(krul_pending_slot_t* slot) {
    uint16_t generation = slot->generation;
    krul_context_release_t release = slot->release;
    void* context = slot->context;
    *slot = (krul_pending_slot_t){.generation = generation};
    if (release != NULL) release(context);
}

krul_pending_slot_t* krul_pending_allocate(
    krul_server_t* server, const krul_command_t* command, uint32_t id,
    void* context, krul_completion_t completion,
    krul_context_release_t release, krul_pending_handle_t* handle) {
    if (server == NULL || !server->initialized || command == NULL ||
        completion == NULL || handle == NULL)
        return NULL;
    for (uint16_t index = 0U; index < server->config.pending_slot_count;
         ++index) {
        krul_pending_slot_t* slot = &server->config.pending_slots[index];
        if (slot->state != KRUL_PENDING_FREE) continue;
        uint16_t generation = (uint16_t)(slot->generation + 1U);
        if (generation == 0U) generation = 1U;
        *slot = (krul_pending_slot_t){.command = command,
                                      .context = context,
                                      .completion = completion,
                                      .release = release,
                                      .id = id,
                                      .generation = generation,
                                      .state = KRUL_PENDING_ACTIVE};
        *handle = (krul_pending_handle_t){.index = index,
                                          .generation = generation};
        return slot;
    }
    return NULL;
}

bool krul_pending_is_active(const krul_server_t* server,
                         krul_pending_handle_t handle) {
    return find_slot(server, handle) != NULL;
}

bool krul_pending_is_waiting(const krul_server_t* server,
                          krul_pending_handle_t handle) {
    krul_pending_slot_t* slot = find_slot(server, handle);
    return slot != NULL && slot->state == KRUL_PENDING_ACTIVE;
}

static bool queue_slot(krul_server_t* server, krul_pending_handle_t handle) {
    krul_pending_slot_t* slot = find_slot(server, handle);
    if (slot == NULL || slot->state != KRUL_PENDING_ACTIVE ||
        server->completion_count >= server->config.pending_slot_count)
        return false;
    server->config.completion_queue[server->completion_tail] = handle.index;
    server->completion_tail = (uint16_t)(
        (server->completion_tail + 1U) % server->config.pending_slot_count);
    ++server->completion_count;
    slot->state = KRUL_PENDING_COMPLETION_QUEUED;
    return true;
}

bool krul_pending_complete(krul_server_t* server,
                        krul_pending_handle_t handle) {
    return queue_slot(server, handle);
}

bool krul_pending_fail(krul_server_t* server,
                       krul_pending_handle_t handle, krul_status_t code,
                       const char* message) {
    krul_pending_slot_t* slot = find_slot(server, handle);
    if (slot == NULL || slot->state != KRUL_PENDING_ACTIVE) return false;
    slot->failed = true;
    slot->failure_code = code;
    (void)snprintf(slot->failure_message, sizeof(slot->failure_message), "%s",
                   message != NULL ? message : "Deferred command failed");
    return queue_slot(server, handle);
}

bool krul_pending_cancel(krul_server_t* server,
                         krul_pending_handle_t handle) {
    krul_pending_slot_t* slot = find_slot(server, handle);
    if (slot == NULL || slot->state != KRUL_PENDING_ACTIVE) return false;
    free_slot(slot);
    return true;
}

bool krul_pending_encode_next(krul_server_t* server) {
    if (server == NULL || !server->initialized) return false;
    if (server->response_ready) return true;
    if (server->completion_count == 0U) return false;

    uint16_t index = server->config.completion_queue[server->completion_head];
    if (index >= server->config.pending_slot_count) return false;
    krul_pending_slot_t* slot = &server->config.pending_slots[index];
    if (slot->state != KRUL_PENDING_COMPLETION_QUEUED) return false;

    size_t size = 0U;
    if (slot->failed) {
        size = krul_encode_error(server, slot->id, slot->failure_code,
                                 slot->failure_message,
                                 server->config.response_buffer,
                                 server->config.response_capacity);
    } else {
        serde_writer_storage_t storage;
        serde_writer_t writer;
        if (begin_response(server, slot->id, &storage, &writer, true)) {
            krul_error_t error = {.code = KRUL_ERROR_EXECUTION};
            krul_result_t result;
            init_result(&result, writer, &error, server, slot->command,
                        slot->id);
            bool success = slot->completion(&result, &error, slot->context);
            if (result.deferred != NULL) {
                krul_pending_handle_t nested = {
                    .index = (uint16_t)(result.deferred -
                                       server->config.pending_slots),
                    .generation = result.deferred->generation};
                (void)krul_pending_cancel(server, nested);
                result.deferred = NULL;
                krul_result_fail(&result,
                                 "A completion callback cannot defer again");
                success = false;
            }
            success = validate_result(&result, success);
            size = finish_response(server, slot->id, writer, &error, success,
                                   true);
        }
    }
    if (size == 0U) return false;
    slot->state = KRUL_PENDING_ENCODED;
    server->response_size = size;
    server->response_slot = index;
    server->response_ready = true;
    return true;
}

bool krul_response_ready(const krul_server_t* server) {
    return server != NULL && server->initialized && server->response_ready;
}

const uint8_t* krul_response_get_data(const krul_server_t* server) {
    return krul_response_ready(server) ? server->config.response_buffer : NULL;
}

size_t krul_response_get_size(const krul_server_t* server) {
    return krul_response_ready(server) ? server->response_size : 0U;
}

void krul_response_release(krul_server_t* server) {
    if (!krul_response_ready(server)) return;
    if (server->response_slot != UINT16_MAX) {
        uint16_t index = server->response_slot;
        if (server->completion_count > 0U &&
            server->config.completion_queue[server->completion_head] == index) {
            server->completion_head = (uint16_t)(
                (server->completion_head + 1U) %
                server->config.pending_slot_count);
            --server->completion_count;
        }
        free_slot(&server->config.pending_slots[index]);
    }
    server->response_ready = false;
    server->response_size = 0U;
    server->response_slot = UINT16_MAX;
}

krul_dispatch_status_t krul_dispatch(krul_server_t* server,
                                     const uint8_t* request,
                                     size_t request_length) {
    uint32_t id = 0U;
    if (server == NULL || !server->initialized || request == NULL)
        return KRUL_DISPATCH_FAILED;
    if (server->response_ready) return KRUL_DISPATCH_BUSY;
    if (server->completion_count > 0U) {
        (void)krul_pending_encode_next(server);
        return KRUL_DISPATCH_BUSY;
    }

    serde_node_t root = {0};
    serde_status_t decode = serde_decode(server->config.codec, request,
                                         request_length, &root);
    if (decode != SERDE_OK ||
        serde_kind(server->config.codec, root) != SERDE_KIND_OBJECT) {
        const char* message = decode == SERDE_ERROR_NO_SPACE
                                  ? "Payload contains too many elements"
                                  : "Malformed request payload";
        return publish_error(server, id, KRUL_ERROR_MALFORMED_PAYLOAD,
                             message);
    }

    krul_error_t error = {.code = KRUL_ERROR_EXECUTION};
    serde_node_t id_node = {0};
    if (!unique_object_get(server->config.codec, root, "id", &id_node,
                           &error, true, NULL))
        return publish_error(server, id, error.code, error.message);
    if (!serde_get_u32(server->config.codec, id_node, &id) || id == 0U)
        return publish_error(server, 0U, KRUL_ERROR_OUT_OF_RANGE,
                             "Field 'id' must be a nonzero uint32");

    serde_node_t command_node = {0};
    char command_name[KRUL_MAX_COMMAND_NAME];
    if (!unique_object_get(server->config.codec, root, "cmd", &command_node,
                           &error, true, NULL))
        return publish_error(server, id, error.code, error.message);
    if (!serde_get_string(server->config.codec, command_node, command_name,
                          sizeof(command_name), NULL))
        return publish_error(server, id, KRUL_ERROR_INVALID_TYPE,
                             "Field 'cmd' must be a short string");
    const krul_command_t* command = krul_find_command(server, command_name);
    if (command == NULL)
        return publish_error(server, id, KRUL_ERROR_UNKNOWN_COMMAND,
                             "Unknown command");

    serde_node_t params_node = {0};
    bool has_params = false;
    (void)unique_object_get(server->config.codec, root, "params", &params_node,
                            &error, false, &has_params);
    if (error.message[0] != '\0')
        return publish_error(server, id, error.code, error.message);
    if (!has_params) {
        for (uint16_t index = 0U; index < command->params_count; ++index) {
            if (!command->params[index].has_default)
                return publish_error(server, id, KRUL_ERROR_MISSING_FIELD,
                                     "Missing field 'params'");
        }
    } else if (!krul_validate_input_object(
                   server->config.codec, params_node, command->params,
                   command->params_count, &error)) {
        return publish_error(server, id, error.code, error.message);
    }

    krul_args_t args = {.codec = server->config.codec,
                        .params = params_node,
                        .command = command,
                        .params_present = has_params};
    serde_writer_storage_t storage;
    serde_writer_t writer;
    bool with_result = !(command->type == KRUL_CMD_BUILTIN &&
                         command->handler == NULL &&
                         strcmp(command->name, "PING") == 0);
    if (!begin_response(server, id, &storage, &writer, with_result))
        return KRUL_DISPATCH_FAILED;

    bool success = false;
    if (command->type == KRUL_CMD_BUILTIN && command->handler == NULL) {
        success = krul_builtin_dispatch(server, command, &args, writer, &error);
    } else {
        krul_result_t result;
        init_result(&result, writer, &error, server, command, id);
        success = command->handler(&args, &result, &error);
        if (result.deferred != NULL) {
            if (success && !result.failed) return KRUL_DISPATCH_DEFERRED;
            krul_pending_handle_t handle = {
                .index = (uint16_t)(result.deferred -
                                   server->config.pending_slots),
                .generation = result.deferred->generation};
            (void)krul_pending_cancel(server, handle);
            result.deferred = NULL;
        }
        success = validate_result(&result, success);
    }
    return publish(server, finish_response(server, id, writer, &error, success,
                                           with_result));
}
