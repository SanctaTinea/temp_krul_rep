#include <stdio.h>
#include <string.h>

#include "ark_emulator_server.h"

static int dispatch_contains(ark_emulator_server_t* emulator,
                             const char* request, const char* expected) {
    krul_server_t* server = ark_emulator_server_krul(emulator);
    if (krul_dispatch(server, (const uint8_t*)request, strlen(request)) !=
        KRUL_DISPATCH_RESPONSE_READY) {
        fprintf(stderr, "No response for: %s\n", request);
        return 1;
    }
    size_t size = krul_response_get_size(server);
    char response[ARK_EMULATOR_FRAME_SIZE + 1U];
    if (size >= sizeof(response)) return 1;
    memcpy(response, krul_response_get_data(server), size);
    response[size] = '\0';
    krul_response_release(server);
    if (strstr(response, expected) == NULL) {
        fprintf(stderr, "Expected '%s' in: %s\n", expected, response);
        return 1;
    }
    return 0;
}

static int dispatch_lacks(ark_emulator_server_t* emulator,
                          const char* request, const char* unexpected) {
    krul_server_t* server = ark_emulator_server_krul(emulator);
    if (krul_dispatch(server, (const uint8_t*)request, strlen(request)) !=
        KRUL_DISPATCH_RESPONSE_READY)
        return 1;
    size_t size = krul_response_get_size(server);
    char response[ARK_EMULATOR_FRAME_SIZE + 1U];
    if (size >= sizeof(response)) return 1;
    memcpy(response, krul_response_get_data(server), size);
    response[size] = '\0';
    krul_response_release(server);
    if (strstr(response, unexpected) != NULL) {
        fprintf(stderr, "Unexpected '%s' in: %s\n", unexpected, response);
        return 1;
    }
    return 0;
}

int main(void) {
    ark_emulator_server_t* emulator = ark_emulator_server_create();
    if (emulator == NULL) return 1;
    int failed = 0;
    failed |= dispatch_contains(
        emulator, "{\"cmd\":\"WHOAMI\",\"id\":1}",
        "\"device_name\":\"АРК\"");
    failed |= dispatch_contains(
        emulator, "{\"cmd\":\"CMD_LIST\",\"id\":2}",
        "\"TEMPERATURE_READ\"");
    failed |= dispatch_lacks(
        emulator, "{\"cmd\":\"CMD_LIST\",\"id\":11}",
        "\"CAN_SELECT\"");
    failed |= dispatch_lacks(
        emulator, "{\"cmd\":\"CMD_LIST\",\"id\":12}",
        "\"PWM_GET_DUTY\"");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"DESCRIBE\",\"params\":{\"name\":\"ADC_READ\"},\"id\":3}",
        "\"widget_hint\":\"special_adc_group\"");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"DAC_SET\",\"params\":{\"channel\":\"DACREF_MCU\",\"value\":1234},\"id\":4}",
        "\"result\":{}");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"DAC_READ\",\"params\":{\"channel\":\"DACREF_MCU\"},\"id\":5}",
        "\"value\":1234");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"DESCRIBE\",\"params\":{\"name\":\"DAC_SET\"},\"id\":8}",
        "\"widget_hint\":\"special_dac\"");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"DESCRIBE\",\"params\":{\"name\":\"DAC_READ\"},\"id\":14}",
        "\"nogui\":true");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"PWM_SET\",\"params\":{\"channel\":\"ARK_PWM1_1\",\"duty_cycle\":25,\"period_counter\":400},\"id\":9}",
        "\"result\":{}");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"CAN_SEND\",\"params\":{\"channel\":\"FDCAN2\",\"id\":256,\"data\":\"ARK\"},\"id\":10}",
        "\"queued\":true");
    failed |= dispatch_lacks(
        emulator,
        "{\"cmd\":\"DESCRIBE\",\"params\":{\"name\":\"QSPI_STATUS\"},\"id\":13}",
        "\"autoupdate\"");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"MEM_WRITE\",\"params\":{\"memory\":\"FRAM\",\"address\":16,\"data\":\"ARK\"},\"id\":6}",
        "\"bytes\":3");
    failed |= dispatch_contains(
        emulator,
        "{\"cmd\":\"MEM_READ\",\"params\":{\"memory\":\"FRAM\",\"address\":16,\"size\":3},\"id\":7}",
        "\"data\":\"41524b\"");
    ark_emulator_server_destroy(emulator);
    return failed == 0 ? 0 : 1;
}
