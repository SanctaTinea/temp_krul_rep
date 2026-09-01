/* Native Krul v4 test server using the production serde and dispatcher. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_handle_t;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_handle_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSE_SOCKET close
#endif

#include "krul.h"
#include "krul_transport.h"
#include "serde_bson.h"
#include "serde_cbor.h"
#include "serde_json.h"

#define HOST_FRAME_CAPACITY (10U * 1024U)
#define HOST_RESPONSE_CAPACITY (16U * 1024U)
#define HOST_DEFAULT_PORT 7001U

typedef struct {
    const char* name;
    const char* type;
    int32_t state;
} mock_pin_t;

static mock_pin_t mock_pins[] = {
    {.name = "BUTTON", .type = "IN", .state = 0},
    {.name = "FAULT", .type = "IN", .state = 0},
    {.name = "LED_GREEN", .type = "OUT", .state = 0},
    {.name = "RELAY", .type = "OUT", .state = 0},
};

static const krul_enum_value_t all_pin_values[] = {
    {.value = 0, .title = "BUTTON"},
    {.value = 1, .title = "FAULT"},
    {.value = 2, .title = "LED_GREEN"},
    {.value = 3, .title = "RELAY"},
};
static const krul_enum_value_t output_pin_values[] = {
    {.value = 0, .title = "LED_GREEN"},
    {.value = 1, .title = "RELAY"},
};
static const krul_enum_value_t pin_type_values[] = {
    {.value = 0, .title = "IN"},
    {.value = 1, .title = "OUT"},
};

static bool write_pin(krul_result_t* result, const mock_pin_t* pin,
                      int32_t name_code, bool include_type) {
    return krul_result_begin_object(result, NULL) &&
           krul_result_put_enum(result, "name", name_code) &&
           (!include_type || krul_result_put_enum(
                                result, "type",
                                strcmp(pin->type, "OUT") == 0 ? 1 : 0)) &&
           krul_result_put_i32(result, "state", pin->state) &&
           krul_result_end_object(result);
}

static bool pin_get_handler(const krul_args_t* args, krul_result_t* result,
                            krul_error_t* error) {
    krul_array_t requested;
    if (!krul_args_get_array(args, "pins", &requested)) return false;
    size_t count = krul_array_get_size(&requested);
    if (!krul_result_begin_array(result, "pins")) return false;
    if (count == 0U) {
        for (size_t index = 0U; index < KRUL_ARRAY_SIZE(mock_pins); ++index)
            if (!write_pin(result, &mock_pins[index], (int32_t)index, true))
                return false;
    } else {
        for (size_t index = 0U; index < count; ++index) {
            int32_t code = 0;
            if (!krul_array_get_enum(&requested, index, &code))
                return false;
            mock_pin_t* pin = code >= 0 &&
                                      (size_t)code < KRUL_ARRAY_SIZE(mock_pins)
                                  ? &mock_pins[code] : NULL;
            if (pin == NULL) {
                krul_error_set(error, KRUL_ERROR_EXECUTION,
                               "Mock pin code %ld is unavailable", (long)code);
                return false;
            }
            if (!write_pin(result, pin, code, true)) return false;
        }
    }
    return krul_result_end_array(result) && krul_result_ok(result);
}

static bool pin_set_handler(const krul_args_t* args, krul_result_t* result,
                            krul_error_t* error) {
    krul_array_t requested;
    if (!krul_args_get_array(args, "pins", &requested)) return false;
    size_t count = krul_array_get_size(&requested);
    mock_pin_t* pins[KRUL_ARRAY_SIZE(output_pin_values)];
    int32_t states[KRUL_ARRAY_SIZE(output_pin_values)];
    for (size_t index = 0U; index < count; ++index) {
        krul_object_t update;
        int32_t code = 0;
        if (!krul_array_get_object(&requested, index, &update) ||
            !krul_object_get_enum(&update, "name", &code) ||
            !krul_object_get_i32(&update, "state", &states[index]))
            return false;
        pins[index] = code >= 0 &&
                              (size_t)code < KRUL_ARRAY_SIZE(output_pin_values)
                          ? &mock_pins[code + 2] : NULL;
        if (pins[index] == NULL) {
            krul_error_set(error, KRUL_ERROR_EXECUTION,
                           "Mock output code %ld is unavailable", (long)code);
            return false;
        }
    }
    if (!krul_result_begin_array(result, "pins")) return false;
    for (size_t index = 0U; index < count; ++index) {
        pins[index]->state = states[index];
        if (!write_pin(result, pins[index],
                       (int32_t)(pins[index] - &mock_pins[2]), false))
            return false;
    }
    return krul_result_end_array(result) && krul_result_ok(result);
}

static bool echo_handler(const krul_args_t* args, krul_result_t* result,
                         krul_error_t* error) {
    (void)error;
    char text[33];
    char output[129];
    int32_t count = 0;
    if (!krul_args_get_string(args, "text", text, sizeof(text)) ||
        !krul_args_get_i32(args, "count", &count))
        return false;
    output[0] = '\0';
    for (int32_t index = 0; index < count; ++index)
        strncat(output, text, sizeof(output) - strlen(output) - 1U);
    return krul_result_put_string(result, "text", output);
}

static uint32_t adc_tick;
static bool adc_read_handler(const krul_args_t* args, krul_result_t* result,
                             krul_error_t* error) {
    (void)args;
    (void)error;
    adc_tick = (adc_tick + 1U) % 4096U;
    return krul_result_begin_object(result, "Voltage") &&
           krul_result_put_i32(result, "value_AIN0", (int32_t)((1000U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN1", (int32_t)((2000U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN2", (int32_t)((3000U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN3", (int32_t)((4000U + adc_tick) % 4096U)) &&
           krul_result_end_object(result) &&
           krul_result_begin_object(result, "Current") &&
           krul_result_put_i32(result, "value_AIN4", (int32_t)((500U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN5", (int32_t)((1500U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN6", (int32_t)((2500U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN7", (int32_t)((3500U + adc_tick) % 4096U)) &&
           krul_result_end_object(result) &&
           krul_result_begin_object(result, "Temp") &&
           krul_result_put_i32(result, "value_AIN8", (int32_t)((750U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN9", (int32_t)((1750U + adc_tick) % 4096U)) &&
           krul_result_end_object(result) &&
           krul_result_put_i32(result, "value_AIN10", (int32_t)((2750U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN11", (int32_t)((3750U + adc_tick) % 4096U)) &&
           krul_result_put_i32(result, "value_AIN12", (int32_t)((2350U + adc_tick) % 4096U));
}

static const krul_field_desc_t describe_params[] = {
    KRUL_STRING_REQUIRED("name", "Command", 1, 47)};
static const krul_field_desc_t echo_params[] = {
    KRUL_STRING_REQUIRED("text", "Text", 1, 32),
    KRUL_I32_DEFAULT("count", "Count", 1, 4, 1)};
static const krul_field_desc_t echo_result[] = {
    KRUL_RESULT_STRING("text", "Result", 128)};

static const krul_field_desc_t pin_get_item = {
    .type = KRUL_TYPE_ENUM,
    .has_constraints = true,
    .constraints.enumeration = {
        .values = all_pin_values,
        .count = (uint16_t)KRUL_ARRAY_SIZE(all_pin_values)}};
static const krul_field_desc_t pin_get_params[] = {{
    .name = "pins",
    .label = "Pins",
    .type = KRUL_TYPE_ARRAY,
    .has_default = true,
    .has_constraints = true,
    .schema.array = {.element = &pin_get_item},
    .constraints.array = {
        .min_count = 0U,
        .max_count = (uint16_t)KRUL_ARRAY_SIZE(all_pin_values)}}};
static const krul_field_desc_t pin_get_result_fields[] = {
    {.name = "name",
     .type = KRUL_TYPE_ENUM,
     .has_constraints = true,
     .constraints.enumeration = {
         .values = all_pin_values,
         .count = (uint16_t)KRUL_ARRAY_SIZE(all_pin_values)}},
    {.name = "type",
     .type = KRUL_TYPE_ENUM,
     .has_constraints = true,
     .constraints.enumeration = {
         .values = pin_type_values,
         .count = (uint16_t)KRUL_ARRAY_SIZE(pin_type_values)}},
    KRUL_I32_REQUIRED("state", "State", 0, 1)};
static const krul_field_desc_t pin_get_result_item = {
    .type = KRUL_TYPE_OBJECT,
    .schema.object = {
        .fields = pin_get_result_fields,
        .count = (uint16_t)KRUL_ARRAY_SIZE(pin_get_result_fields)}};
static const krul_field_desc_t pin_get_result[] = {{
    .name = "pins",
    .type = KRUL_TYPE_ARRAY,
    .has_constraints = true,
    .schema.array = {.element = &pin_get_result_item},
    .constraints.array = {
        .min_count = 0U,
        .max_count = (uint16_t)KRUL_ARRAY_SIZE(all_pin_values)}}};

static const krul_field_desc_t pin_set_params_fields[] = {
    {.name = "name",
     .type = KRUL_TYPE_ENUM,
     .has_constraints = true,
     .constraints.enumeration = {
         .values = output_pin_values,
         .count = (uint16_t)KRUL_ARRAY_SIZE(output_pin_values)}},
    KRUL_I32_REQUIRED("state", "State", 0, 1)};
static const krul_field_desc_t pin_set_param_item = {
    .type = KRUL_TYPE_OBJECT,
    .schema.object = {
        .fields = pin_set_params_fields,
        .count = (uint16_t)KRUL_ARRAY_SIZE(pin_set_params_fields)}};
static const krul_field_desc_t pin_set_params[] = {{
    .name = "pins",
    .type = KRUL_TYPE_ARRAY,
    .has_constraints = true,
    .schema.array = {.element = &pin_set_param_item},
    .constraints.array = {
        .min_count = 1U,
        .max_count = (uint16_t)KRUL_ARRAY_SIZE(output_pin_values)}}};
static const krul_field_desc_t pin_set_result_fields[] = {
    {.name = "name",
     .type = KRUL_TYPE_ENUM,
     .has_constraints = true,
     .constraints.enumeration = {
         .values = output_pin_values,
         .count = (uint16_t)KRUL_ARRAY_SIZE(output_pin_values)}},
    KRUL_I32_REQUIRED("state", "State", 0, 1)};
static const krul_field_desc_t pin_set_result_item = {
    .type = KRUL_TYPE_OBJECT,
    .schema.object = {
        .fields = pin_set_result_fields,
        .count = (uint16_t)KRUL_ARRAY_SIZE(pin_set_result_fields)}};
static const krul_field_desc_t pin_set_result[] = {{
    .name = "pins",
    .type = KRUL_TYPE_ARRAY,
    .has_constraints = true,
    .schema.array = {.element = &pin_set_result_item},
    .constraints.array = {
        .min_count = 1U,
        .max_count = (uint16_t)KRUL_ARRAY_SIZE(output_pin_values)}}};
static const krul_field_desc_t adc_voltage_fields[] = {
    KRUL_RESULT_I32("value_AIN0", "AIN0"),
    KRUL_RESULT_I32("value_AIN1", "AIN1"),
    KRUL_RESULT_I32("value_AIN2", "AIN2"),
    KRUL_RESULT_I32("value_AIN3", "AIN3")};
static const krul_field_desc_t adc_current_fields[] = {
    KRUL_RESULT_I32("value_AIN4", "AIN4"),
    KRUL_RESULT_I32("value_AIN5", "AIN5"),
    KRUL_RESULT_I32("value_AIN6", "AIN6"),
    KRUL_RESULT_I32("value_AIN7", "AIN7")};
static const krul_field_desc_t adc_temp_fields[] = {
    KRUL_RESULT_I32("value_AIN8", "AIN8"),
    KRUL_RESULT_I32("value_AIN9", "AIN9")};
static const krul_field_desc_t adc_result[] = {
    {.name = "Voltage",
     .label = "Voltage",
     .type = KRUL_TYPE_OBJECT,
     .widget = KRUL_WIDGET_SPECIAL_ADC_GROUP,
     .schema.object = {
         .fields = adc_voltage_fields,
         .count = (uint16_t)KRUL_ARRAY_SIZE(adc_voltage_fields)}},
    {.name = "Current",
     .label = "Current",
     .type = KRUL_TYPE_OBJECT,
     .widget = KRUL_WIDGET_SPECIAL_ADC_GROUP,
     .schema.object = {
         .fields = adc_current_fields,
         .count = (uint16_t)KRUL_ARRAY_SIZE(adc_current_fields)}},
    {.name = "Temp",
     .label = "Temperature",
     .type = KRUL_TYPE_OBJECT,
     .widget = KRUL_WIDGET_SPECIAL_ADC_GROUP,
     .schema.object = {
         .fields = adc_temp_fields,
         .count = (uint16_t)KRUL_ARRAY_SIZE(adc_temp_fields)}},
    {.name = "value_AIN10",
     .label = "AIN10",
     .type = KRUL_TYPE_I32,
     .widget = KRUL_WIDGET_SPECIAL_ADC},
    {.name = "value_AIN11",
     .label = "AIN11",
     .type = KRUL_TYPE_I32,
     .widget = KRUL_WIDGET_SPECIAL_ADC},
    {.name = "value_AIN12",
     .label = "AIN12",
     .type = KRUL_TYPE_I32,
     .widget = KRUL_WIDGET_SPECIAL_ADC}};

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
static const krul_command_t pin_get = {
    .name = "PIN_GET",
    .type = KRUL_CMD_NORMAL,
    .widget = KRUL_WIDGET_SPECIAL_GPIO,
    .params = pin_get_params,
    .params_count = KRUL_ARRAY_SIZE(pin_get_params),
    .result = pin_get_result,
    .result_count = KRUL_ARRAY_SIZE(pin_get_result),
    .handler = pin_get_handler};
static const krul_command_t pin_set = {
    .name = "PIN_SET",
    .type = KRUL_CMD_NORMAL,
    .widget = KRUL_WIDGET_SPECIAL_GPIO,
    .params = pin_set_params,
    .params_count = KRUL_ARRAY_SIZE(pin_set_params),
    .result = pin_set_result,
    .result_count = KRUL_ARRAY_SIZE(pin_set_result),
    .handler = pin_set_handler};
static const krul_command_t echo = {
    .name = "ECHO",
    .tab = "PC simulator",
    .title = "Echo",
    .group = "Protocol",
    .order = 10U,
    .params = echo_params,
    .params_count = KRUL_ARRAY_SIZE(echo_params),
    .result = echo_result,
    .result_count = KRUL_ARRAY_SIZE(echo_result),
    .handler = echo_handler};
static const krul_command_t adc_read = {
    .name = "ADC_READ",
    .tab = "PC simulator",
    .title = "Virtual ADC",
    .group = "Monitoring",
    .order = 20U,
    .result = adc_result,
    .result_count = KRUL_ARRAY_SIZE(adc_result),
    .autoupdate = true,
    .min_period_ms = 100U,
    .max_period_ms = 2000U,
    .default_period_ms = 500U,
    .handler = adc_read_handler};
static const krul_command_t* const commands[] = {
    &ping, &whoami, &command_list, &describe, &pin_get, &pin_set, &echo,
    &adc_read};

static krul_server_t json_server;
static krul_server_t bson_server;
static krul_server_t cbor_server;
static uint8_t json_server_response[HOST_RESPONSE_CAPACITY];
static uint8_t bson_server_response[HOST_RESPONSE_CAPACITY];
static uint8_t cbor_server_response[HOST_RESPONSE_CAPACITY];

static bool send_all(socket_handle_t socket, const uint8_t* data,
                     size_t size) {
    while (size > 0U) {
        int chunk = send(socket, (const char*)data,
                         size > 16384U ? 16384 : (int)size, 0);
        if (chunk == SOCKET_ERROR || chunk == 0) return false;
        data += (size_t)chunk;
        size -= (size_t)chunk;
    }
    return true;
}

static krul_server_t* server_for_format(krul_transport_format_t format) {
    if (format == KRUL_TRANSPORT_FORMAT_JSON) return &json_server;
    if (format == KRUL_TRANSPORT_FORMAT_BSON) return &bson_server;
    if (format == KRUL_TRANSPORT_FORMAT_CBOR) return &cbor_server;
    return NULL;
}

static size_t dispatch_frame(krul_transport_format_t format,
                             const uint8_t* payload, size_t payload_size,
                             uint8_t* response, size_t capacity) {
    krul_server_t* server = server_for_format(format);
    if (server == NULL || krul_dispatch(server, payload, payload_size) !=
        KRUL_DISPATCH_RESPONSE_READY)
        return 0U;
    size_t size = krul_transport_encode(
        format, krul_response_get_data(server), krul_response_get_size(server),
        response, capacity);
    krul_response_release(server);
    return size;
}

static bool serve_socket_client(socket_handle_t client) {
    uint8_t payload[HOST_FRAME_CAPACITY];
    uint8_t input[1024];
    uint8_t response[HOST_RESPONSE_CAPACITY + KRUL_TRANSPORT_OVERHEAD];
    krul_transport_parser_t parser;
    if (!krul_transport_parser_init(&parser, payload, sizeof(payload)))
        return false;
    for (;;) {
        int received = recv(client, (char*)input, sizeof(input), 0);
        if (received <= 0) return true;
        size_t offset = 0U;
        while (offset < (size_t)received) {
            size_t consumed = 0U;
            krul_transport_status_t status = krul_transport_parser_consume(
                &parser, input + offset, (size_t)received - offset, &consumed);
            offset += consumed;
            if (status == KRUL_TRANSPORT_FRAME_READY) {
                size_t response_size = dispatch_frame(
                    parser.format, parser.payload, parser.payload_size,
                    response, sizeof(response));
                if (response_size == 0U ||
                    !send_all(client, response, response_size))
                    return false;
                krul_transport_parser_reset(&parser);
            } else if (status == KRUL_TRANSPORT_INVALID_STATE) {
                return false;
            } else if (status == KRUL_TRANSPORT_NEED_MORE) {
                break;
            }
        }
    }
}

static int serve_stdio(void) {
    uint8_t payload[HOST_FRAME_CAPACITY];
    uint8_t input[1024];
    uint8_t response[HOST_RESPONSE_CAPACITY + KRUL_TRANSPORT_OVERHEAD];
    krul_transport_parser_t parser;
    if (!krul_transport_parser_init(&parser, payload, sizeof(payload))) return 1;
    for (;;) {
        size_t received = fread(input, 1U, sizeof(input), stdin);
        if (received == 0U) return ferror(stdin) ? 1 : 0;
        size_t offset = 0U;
        while (offset < received) {
            size_t consumed = 0U;
            krul_transport_status_t status = krul_transport_parser_consume(
                &parser, input + offset, received - offset, &consumed);
            offset += consumed;
            if (status == KRUL_TRANSPORT_FRAME_READY) {
                size_t response_size = dispatch_frame(
                    parser.format, parser.payload, parser.payload_size,
                    response, sizeof(response));
                if (response_size == 0U ||
                    fwrite(response, 1U, response_size, stdout) !=
                        response_size ||
                    fflush(stdout) != 0)
                    return 1;
                krul_transport_parser_reset(&parser);
            } else if (status == KRUL_TRANSPORT_INVALID_STATE) {
                return 1;
            } else if (status == KRUL_TRANSPORT_NEED_MORE) {
                break;
            }
        }
    }
}

static int serve_tcp(uint16_t port) {
#ifdef _WIN32
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        fputs("WSAStartup failed\n", stderr);
        return 1;
    }
#endif
    socket_handle_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        fputs("Cannot create listening socket\n", stderr);
        return 1;
    }
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     (const char*)&reuse, (int)sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, (const struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        fprintf(stderr, "Cannot listen on 127.0.0.1:%u\n", (unsigned)port);
        CLOSE_SOCKET(listener);
        return 1;
    }
    fprintf(stderr, "Native Krul server listening on 127.0.0.1:%u\n",
            (unsigned)port);
    for (;;) {
        socket_handle_t client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        (void)serve_socket_client(client);
        CLOSE_SOCKET(client);
    }
    CLOSE_SOCKET(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

static bool parse_port(const char* text, uint16_t* port) {
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1L || value > 65535L)
        return false;
    *port = (uint16_t)value;
    return true;
}

int main(int argc, char** argv) {
    bool stdio_mode = false;
    uint16_t port = HOST_DEFAULT_PORT;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--stdio") == 0) {
            stdio_mode = true;
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (!parse_port(argv[++index], &port)) {
                fputs("Invalid --port value\n", stderr);
                return 2;
            }
        } else {
            fprintf(stderr, "Usage: %s [--stdio] [--port PORT]\n", argv[0]);
            return 2;
        }
    }

    serde_json_token_t tokens[SERDE_JSON_DEFAULT_TOKEN_COUNT];
    serde_json_codec_t json;
    serde_bson_codec_t bson;
    serde_cbor_codec_t cbor;
    if (!serde_json_init(&json, tokens, KRUL_ARRAY_SIZE(tokens)) ||
        !serde_bson_init(&bson) || !serde_cbor_init(&cbor))
        return 1;
    krul_server_config_t config = {
        .commands = commands,
        .command_count = KRUL_ARRAY_SIZE(commands),
        .device_name = "ARK-PC-NATIVE",
        .firmware_version = "host-1.0.0",
        .protocol_version = 4U,
        .codec = serde_json(&json),
        .response_buffer = json_server_response,
        .response_capacity = sizeof(json_server_response)};
    krul_server_config_t bson_config = config;
    bson_config.codec = serde_bson(&bson);
    bson_config.response_buffer = bson_server_response;
    krul_server_config_t cbor_config = config;
    cbor_config.codec = serde_cbor(&cbor);
    cbor_config.response_buffer = cbor_server_response;
    if (!krul_server_init(&json_server, &config) ||
        !krul_server_init(&bson_server, &bson_config) ||
        !krul_server_init(&cbor_server, &cbor_config)) {
        fputs("Invalid Krul descriptor table\n", stderr);
        return 1;
    }
    return stdio_mode ? serve_stdio() : serve_tcp(port);
}
