#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "krul.h"
#include "serde_bson.h"
#include "serde_cbor.h"
#include "serde_json.h"

#define assert(condition)                                                   \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "assertion failed at line %d: %s\n", __LINE__, \
                    #condition);                                            \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

static int32_t deferred_values[2];
static krul_pending_handle_t deferred_handles[2];
static size_t deferred_next;
static unsigned int release_count;
static bool deferred_completion(krul_result_t* result, krul_error_t* error,
                                void* context);

static void deferred_release(void* context) {
    assert(context == &deferred_values[0] || context == &deferred_values[1]);
    ++release_count;
}

static bool echo_handler(const krul_args_t* args, krul_result_t* result,
                         krul_error_t* error) {
    (void)error;
    char text[17];
    int32_t count = 0;
    assert(krul_args_get_string(args, "text", text, sizeof(text)));
    assert(krul_args_get_i32(args, "count", &count));
    krul_result_put_string(result, "text", text);
    krul_result_put_i32(result, "count", count);
    return true;
}

static bool bad_missing_handler(const krul_args_t* args,
                                krul_result_t* result,
                                krul_error_t* error) {
    (void)args;
    (void)error;
    krul_result_put_i32(result, "first", 1);
    return true;
}

static bool bad_type_handler(const krul_args_t* args, krul_result_t* result,
                             krul_error_t* error) {
    (void)args;
    (void)error;
    krul_result_put_bool(result, "value", true);
    return true;
}

static bool array_handler(const krul_args_t* args, krul_result_t* result,
                          krul_error_t* error) {
    (void)error;
    krul_array_t items;
    krul_object_t item;
    char name[16];
    int32_t state = -1;
    assert(krul_args_get_array(args, "items", &items));
    assert(krul_array_get_size(&items) == 2U);
    assert(krul_array_get_object(&items, 1U, &item));
    assert(krul_object_get_string(&item, "name", name, sizeof(name)));
    assert(krul_object_get_i32(&item, "state", &state));
    krul_result_put_string(result, "name", name);
    krul_result_put_i32(result, "state", state);
    return true;
}

static bool deferred_handler(const krul_args_t* args, krul_result_t* result,
                             krul_error_t* error) {
    (void)args;
    if (deferred_next >= KRUL_ARRAY_SIZE(deferred_values)) {
        krul_error_set(error, KRUL_ERROR_EXECUTION,
                       "No application context is available");
        return false;
    }
    size_t index = deferred_next;
    if (!krul_result_defer(result, &deferred_values[index],
                           deferred_completion, deferred_release,
                           &deferred_handles[index]))
        return false;
    ++deferred_next;
    return true;
}

static bool deferred_completion(krul_result_t* result, krul_error_t* error,
                                void* context) {
    (void)error;
    return krul_result_put_i32(result, "value", *(const int32_t*)context);
}

static const krul_field_desc_t describe_params[] = {
    KRUL_STRING_REQUIRED("name", "Command", 1, 47)};
static const krul_field_desc_t echo_params[] = {
    KRUL_STRING_REQUIRED("text", "Text", 1, 16),
    KRUL_I32_DEFAULT("count", "Count", 1, 10, 2)};
static const krul_field_desc_t echo_result[] = {
    KRUL_RESULT_STRING("text", "Text", 16),
    KRUL_I32_REQUIRED("count", "Count", 1, 10)};
static const krul_field_desc_t pair_result[] = {
    KRUL_RESULT_I32("first", "First"), KRUL_RESULT_I32("second", "Second")};
static const krul_field_desc_t value_result[] = {
    KRUL_RESULT_I32("value", "Value")};

static const krul_field_desc_t array_object_fields[] = {
    KRUL_STRING_REQUIRED("name", "Name", 1, 15),
    KRUL_I32_REQUIRED("state", "State", 0, 1)};
static const krul_field_desc_t array_object = {
    .type = KRUL_TYPE_OBJECT,
    .schema.object = {
        .fields = array_object_fields,
        .count = KRUL_ARRAY_SIZE(array_object_fields)}};
static const krul_field_desc_t array_params[] = {{
    .name = "items",
    .type = KRUL_TYPE_ARRAY,
    .has_constraints = true,
    .schema.array = {.element = &array_object},
    .constraints.array = {
        .min_count = 2U, .max_count = 2U}}};
static const krul_field_desc_t array_result[] = {
    KRUL_RESULT_STRING("name", "Name", 15),
    KRUL_I32_REQUIRED("state", "State", 0, 1)};

static const krul_command_t whoami = {
    .name = "WHOAMI", .type = KRUL_CMD_BUILTIN};
static const krul_command_t ping = {
    .name = "PING", .type = KRUL_CMD_BUILTIN};
static const krul_command_t command_list = {
    .name = "CMD_LIST", .type = KRUL_CMD_BUILTIN};
static const krul_command_t describe = {
    .name = "DESCRIBE",
    .type = KRUL_CMD_BUILTIN,
    .params = describe_params,
    .params_count = KRUL_ARRAY_SIZE(describe_params)};
static const krul_command_t echo = {
    .name = "ECHO",
    .title = "Echo",
    .description = "Repeat the text; the rabbit handles the rest.",
    .params = echo_params,
    .params_count = KRUL_ARRAY_SIZE(echo_params),
    .result = echo_result,
    .result_count = KRUL_ARRAY_SIZE(echo_result),
    .handler = echo_handler};
static const krul_command_t hidden = {
    .name = "HIDDEN",
    .type = KRUL_CMD_NOGUI,
    .params = echo_params,
    .params_count = KRUL_ARRAY_SIZE(echo_params),
    .result = echo_result,
    .result_count = KRUL_ARRAY_SIZE(echo_result),
    .handler = echo_handler};
static const krul_command_t array = {
    .name = "ARRAY",
    .params = array_params,
    .params_count = KRUL_ARRAY_SIZE(array_params),
    .result = array_result,
    .result_count = KRUL_ARRAY_SIZE(array_result),
    .handler = array_handler};
static const krul_command_t bad_missing = {
    .name = "BAD_MISSING",
    .result = pair_result,
    .result_count = KRUL_ARRAY_SIZE(pair_result),
    .handler = bad_missing_handler};
static const krul_command_t bad_type = {
    .name = "BAD_TYPE",
    .result = value_result,
    .result_count = KRUL_ARRAY_SIZE(value_result),
    .handler = bad_type_handler};
static const krul_command_t deferred = {
    .name = "DEFERRED",
    .result = value_result,
    .result_count = KRUL_ARRAY_SIZE(value_result),
    .timeout_ms = 12000U,
    .handler = deferred_handler};
static const krul_command_t* const commands[] = {
    &ping, &whoami, &command_list, &describe, &echo, &hidden,
    &array,  &bad_missing,  &bad_type, &deferred};

static krul_server_t server;

static void expect_contains(const char* request, const char* expected) {
    krul_dispatch_status_t status = krul_dispatch(
        &server, (const uint8_t*)request, strlen(request));
    assert(status == KRUL_DISPATCH_RESPONSE_READY);
    const uint8_t* response = krul_response_get_data(&server);
    size_t length = krul_response_get_size(&server);
    assert(length > 0U);
    assert(strstr((const char*)response, expected) != NULL);
    krul_response_release(&server);
}

int main(void) {
    serde_json_token_t tokens[SERDE_JSON_DEFAULT_TOKEN_COUNT];
    serde_json_codec_t json;
    uint8_t response[4096];
    krul_pending_slot_t pending_slots[2];
    uint16_t completion_queue[2];
    assert(serde_json_init(&json, tokens, KRUL_ARRAY_SIZE(tokens)));
    krul_server_config_t config = {
        .commands = commands,
        .command_count = KRUL_ARRAY_SIZE(commands),
        .device_name = "TEST",
        .device_id = "TEST-DEVICE-01",
        .firmware_version = "2.0.0",
        .protocol_version = 4,
        .codec = serde_json(&json),
        .response_buffer = response,
        .response_capacity = sizeof(response),
        .pending_slots = pending_slots,
        .pending_slot_count = KRUL_ARRAY_SIZE(pending_slots),
        .completion_queue = completion_queue};
    assert(krul_server_init(&server, &config));

    uint8_t log_event[256];
    size_t log_length = krul_encode_log_eventf(
        &server, KRUL_CONSOLE_WARNING, log_event, sizeof(log_event),
        "temperature=%d C", 42);
    assert(log_length > 0U);
    assert(strstr((const char*)log_event, "\"event\":\"log\"") != NULL);
    assert(strstr((const char*)log_event,
                  "\"severity\":\"warning\"") != NULL);
    assert(strstr((const char*)log_event,
                  "\"message\":\"temperature=42 C\"") != NULL);

    expect_contains("{\"cmd\":\"WHOAMI\",\"id\":1}",
                    "\"device_name\":\"TEST\"");
    expect_contains("{\"cmd\":\"WHOAMI\",\"id\":10}",
                    "\"device_id\":\"TEST-DEVICE-01\"");
    expect_contains("{\"cmd\":\"WHOAMI\",\"id\":11}",
                    "\"protocol_version\":4");
    expect_contains("{\"cmd\":\"PING\",\"id\":12}",
                    "{\"id\":12,\"success\":true}");
    expect_contains("{\"cmd\":\"CMD_LIST\",\"id\":2}", "\"ECHO\"");
    expect_contains("{\"cmd\":\"CMD_LIST\",\"id\":21}", "\"HIDDEN\"");
    expect_contains(
        "{\"cmd\":\"DESCRIBE\",\"id\":3,\"params\":{\"name\":\"ECHO\"}}",
        "\"default\":2");
    expect_contains(
        "{\"cmd\":\"DESCRIBE\",\"id\":31,\"params\":{\"name\":\"ECHO\"}}",
        "\"description\":\"Repeat the text; the rabbit handles the rest.\"");
    expect_contains(
        "{\"cmd\":\"DESCRIBE\",\"id\":32,\"params\":{\"name\":\"HIDDEN\"}}",
        "\"nogui\":true");
    expect_contains(
        "{\"cmd\":\"HIDDEN\",\"id\":33,\"params\":{\"text\":\"secret\"}}",
        "\"text\":\"secret\"");
    expect_contains(
        "{\"cmd\":\"ECHO\",\"id\":4,\"params\":{\"text\":\"hi\\nthere\"}}",
        "\"count\":2");
    expect_contains("{\"cmd\":\"ECHO\",\"id\":5,\"params\":{}}",
                    "\"code\":1");
    expect_contains(
        "{\"cmd\":\"ECHO\",\"id\":6,\"params\":{\"text\":\"x\",\"count\":99}}",
        "\"code\":3");
    expect_contains(
        "{\"cmd\":\"ECHO\",\"id\":7,\"params\":{\"text\":\"x\",\"extra\":1}}",
        "\"count\":2");
    expect_contains(
        "{\"cmd\":\"WHOAMI\",\"id\":71,\"extension\":true}",
        "\"device_name\":\"TEST\"");
    expect_contains(
        "{\"cmd\":\"ECHO\",\"id\":72,\"params\":{\"text\":\"x\",\"count\":2,\"count\":3}}",
        "\"code\":5");
    expect_contains(
        "{\"cmd\":\"ARRAY\",\"id\":8,\"params\":{\"items\":[{\"name\":\"A\",\"state\":0,\"extension\":false},{\"name\":\"B\",\"state\":1}]}}",
        "\"name\":\"B\",\"state\":1");
    expect_contains(
        "{\"cmd\":\"ARRAY\",\"id\":9,\"params\":{\"items\":[{\"name\":\"A\",\"state\":2},{\"name\":\"B\",\"state\":1}]}}",
        "\"code\":3");
    expect_contains("{\"cmd\":\"BAD_MISSING\",\"id\":10}",
                    "\"code\":9");
    expect_contains("{\"cmd\":\"BAD_TYPE\",\"id\":11}",
                    "\"code\":9");
    expect_contains(
        "{\"cmd\":\"DESCRIBE\",\"id\":12,\"params\":{\"name\":\"DEFERRED\"}}",
        "\"timeout_ms\":12000");

    const char* deferred_request = "{\"cmd\":\"DEFERRED\",\"id\":13}";
    assert(krul_dispatch(&server, (const uint8_t*)deferred_request,
                         strlen(deferred_request)) == KRUL_DISPATCH_DEFERRED);
    assert(krul_pending_is_active(&server, deferred_handles[0]));
    deferred_values[0] = 42;
    assert(krul_pending_complete(&server, deferred_handles[0]));
    assert(krul_pending_encode_next(&server));
    assert(strstr((const char*)krul_response_get_data(&server), "\"id\":13") != NULL);
    assert(strstr((const char*)krul_response_get_data(&server),
                  "\"value\":42") != NULL);
    assert(krul_pending_is_active(&server, deferred_handles[0]));
    krul_pending_handle_t first_handle = deferred_handles[0];
    krul_response_release(&server);
    assert(!krul_pending_is_active(&server, deferred_handles[0]));
    assert(release_count == 1U);

    deferred_next = 0U;
    assert(krul_dispatch(&server, (const uint8_t*)deferred_request,
                         strlen(deferred_request)) == KRUL_DISPATCH_DEFERRED);
    assert(deferred_handles[0].index == first_handle.index);
    assert(deferred_handles[0].generation != first_handle.generation);
    assert(!krul_pending_is_active(&server, first_handle));
    assert(krul_pending_fail(&server, deferred_handles[0], KRUL_ERROR_EXECUTION,
                             "deferred failure"));
    assert(krul_pending_encode_next(&server));
    assert(strstr((const char*)krul_response_get_data(&server),
                  "deferred failure") != NULL);
    krul_response_release(&server);
    assert(release_count == 2U);

    deferred_next = 0U;
    const char* deferred_request_16 = "{\"cmd\":\"DEFERRED\",\"id\":16}";
    const char* deferred_request_17 = "{\"cmd\":\"DEFERRED\",\"id\":17}";
    assert(krul_dispatch(&server, (const uint8_t*)deferred_request_16,
                         strlen(deferred_request_16)) == KRUL_DISPATCH_DEFERRED);
    assert(krul_dispatch(&server, (const uint8_t*)deferred_request_17,
                         strlen(deferred_request_17)) == KRUL_DISPATCH_DEFERRED);
    deferred_values[0] = 16;
    deferred_values[1] = 17;
    assert(krul_pending_complete(&server, deferred_handles[1]));
    assert(krul_pending_complete(&server, deferred_handles[0]));
    assert(krul_pending_encode_next(&server));
    assert(strstr((const char*)krul_response_get_data(&server), "\"id\":17") != NULL);
    krul_response_release(&server);
    assert(krul_pending_encode_next(&server));
    assert(strstr((const char*)krul_response_get_data(&server), "\"id\":16") != NULL);
    krul_response_release(&server);
    assert(release_count == 4U);

    assert(krul_dispatch(&server, (const uint8_t*)"{\"cmd\":\"WHOAMI\",\"id\":14}",
                         strlen("{\"cmd\":\"WHOAMI\",\"id\":14}")) ==
           KRUL_DISPATCH_RESPONSE_READY);
    assert(krul_dispatch(&server, (const uint8_t*)"{\"cmd\":\"WHOAMI\",\"id\":15}",
                         strlen("{\"cmd\":\"WHOAMI\",\"id\":15}")) ==
           KRUL_DISPATCH_BUSY);
    krul_response_release(&server);
    expect_contains("{bad", "\"code\":4");

    /* The same Krul dispatcher and command table operate over BSON unchanged. */
    serde_bson_codec_t bson;
    assert(serde_bson_init(&bson));
    config.codec = serde_bson(&bson);
    assert(krul_server_init(&server, &config));
    uint8_t bson_request[64];
    serde_writer_storage_t bson_storage;
    serde_writer_t bson_writer;
    assert(serde_writer_open(config.codec, &bson_storage, bson_request,
                             sizeof(bson_request), &bson_writer));
    assert(serde_begin_object(bson_writer, NULL));
    assert(serde_put_string(
        bson_writer, &(const serde_key_t){.name = "cmd"}, "WHOAMI"));
    assert(serde_put_u32(bson_writer, &(const serde_key_t){.name = "id"},
                         101U));
    assert(serde_end_object(bson_writer));
    size_t bson_request_size = 0U;
    assert(serde_writer_finish(bson_writer, &bson_request_size));
    assert(krul_dispatch(&server, bson_request, bson_request_size) ==
           KRUL_DISPATCH_RESPONSE_READY);
    serde_node_t bson_root = {0};
    assert(serde_decode(config.codec, krul_response_get_data(&server),
                        krul_response_get_size(&server), &bson_root) ==
           SERDE_OK);
    serde_node_t bson_id = {0};
    serde_node_t bson_result = {0};
    serde_node_t bson_device = {0};
    uint32_t bson_id_value = 0U;
    char bson_device_name[16];
    assert(serde_object_get(config.codec, bson_root,
                            &(const serde_key_t){.name = "id"}, &bson_id,
                            NULL));
    assert(serde_get_u32(config.codec, bson_id, &bson_id_value) &&
           bson_id_value == 101U);
    assert(serde_object_get(config.codec, bson_root,
                            &(const serde_key_t){.name = "result"},
                            &bson_result, NULL));
    assert(serde_object_get(config.codec, bson_result,
                            &(const serde_key_t){.name = "device_name"},
                            &bson_device, NULL));
    assert(serde_get_string(config.codec, bson_device, bson_device_name,
                            sizeof(bson_device_name), NULL));
    assert(strcmp(bson_device_name, "TEST") == 0);
    krul_response_release(&server);

    /* Compact CBOR uses numeric tags and indefinite-length containers. */
    serde_cbor_codec_t cbor;
    assert(serde_cbor_init(&cbor));
    config.codec = serde_cbor(&cbor);
    assert(krul_server_init(&server, &config));
    uint8_t cbor_request[64];
    serde_writer_storage_t cbor_storage;
    serde_writer_t cbor_writer;
    assert(serde_writer_open(config.codec, &cbor_storage, cbor_request,
                             sizeof(cbor_request), &cbor_writer));
    serde_key_t cbor_cmd = {.name = "cmd", .tag = serde_key_tag("cmd")};
    serde_key_t cbor_id_key = {.name = "id", .tag = serde_key_tag("id")};
    assert(serde_begin_object(cbor_writer, NULL));
    assert(serde_put_string(cbor_writer, &cbor_cmd, "WHOAMI"));
    assert(serde_put_u32(cbor_writer, &cbor_id_key, 102U));
    assert(serde_end_object(cbor_writer));
    size_t cbor_request_size = 0U;
    assert(serde_writer_finish(cbor_writer, &cbor_request_size));
    assert(cbor_request[0] == 0xbfU);
    assert(krul_dispatch(&server, cbor_request, cbor_request_size) ==
           KRUL_DISPATCH_RESPONSE_READY);
    serde_node_t cbor_root = {0};
    assert(serde_decode(config.codec, krul_response_get_data(&server),
                        krul_response_get_size(&server), &cbor_root) ==
           SERDE_OK);
    serde_node_t cbor_id = {0};
    uint32_t cbor_id_value = 0U;
    assert(serde_object_get(config.codec, cbor_root, &cbor_id_key, &cbor_id,
                            NULL));
    assert(serde_get_u32(config.codec, cbor_id, &cbor_id_value) &&
           cbor_id_value == 102U);
    krul_response_release(&server);

    puts("krul_host_test: OK");
    return 0;
}
