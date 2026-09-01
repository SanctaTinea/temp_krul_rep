#include "ark_emulator_server.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serde_json.h"
#include "serde_bson.h"
#include "serde_cbor.h"

#define ARRAY_SIZE(items) (sizeof(items) / sizeof((items)[0]))
#define EXTERNAL_MEMORY_SIZE (1U << 21)
#define MEM_MAX_BUFFER 200U

#define ITCM_START 0x00000000UL
#define ITCM_SIZE 0x00010000UL
#define DTCM_START 0x20000000UL
#define DTCM_SIZE 0x00020000UL
#define AXI_SRAM_START 0x24000000UL
#define AXI_SRAM_SIZE 0x00080000UL
#define D2_SRAM_START 0x30000000UL
#define D2_SRAM_SIZE 0x00048000UL
#define D3_SRAM_START 0x38000000UL
#define D3_SRAM_SIZE 0x00010000UL
#define FLASH_START 0x08000000UL
#define FLASH_SIZE 0x00200000UL

#define OUTPUT_PIN_LIST(X) \
    X(PBUS_MEA_ON) X(ARK_SW_CH2_2) X(ARK_SW_CH2_1) X(TLM_PWR_EN) \
    X(BKU4_S) X(ARK_SW_CH5_1) X(EN_TRG_H2) X(MEASB1S) X(MEASB2S) \
    X(MEASB3S) X(MEASBAT) X(ARK_CHARGE_1) X(ARK_SW_CH5_2) \
    X(SOTR_A_PWR_EN) X(SOTR_M_PWR_EN) X(LED_1) X(LED_2) X(LED_3) \
    X(LED_4) X(BKU2_S) X(BAL1S_EN) X(BAL2S_EN) X(BAL3S_EN) \
    X(BAL4S_EN) X(BKU1_S) X(EN_TRG_H1) X(BKU3_R) X(ARK_SW_CH4_1) \
    X(BKU2_R) X(BKU1_R) X(ARK_SW_CH3_1) X(ARK_SW_CH3_2) \
    X(ARK_CHARGE_2) X(ARK_SW_CH1_2) X(MCU_1W_T_EN) \
    X(MCU_1WR_THERM1_3) X(ARK_OFF_5V_1) X(ARK_OFF_5V_2) \
    X(ARK_SW_CH1_1) X(BAL_EN) X(BKU4_R) X(MCU_SEL_13_46THM) \
    X(SOLTH1_PWEN) X(SOLTH2_PWEN) X(MCU_SEL_ATHM) X(BKU3_S) \
    X(OFFMK_3) X(OFFMK_2) X(OFFMK_1) X(OPEN_KEYS) X(ARK_SW_CH4_2)

#define INPUT_PIN_LIST(X) \
    X(ADD_MCU_b1) X(ADD_MCU_b2) X(ADD_MCU_b3) X(ADD_MCU_b4) \
    X(ADD_MCU_b6) X(ADD_MCU_b7_parity) X(BALMH1_PWFAIL) \
    X(BALMH2_PWFAIL) X(CATCH_1W) X(CATCH_CS) X(FPF_CAN1_FLAGB) \
    X(FPF_CAN2_FLAGB) X(FPF_FRAM_FLAGB) X(FPF_MRAM_FLAGB) \
    X(FPF_PWRBUF_FLAGB) X(FPF_PWRI2C_FLAGB) X(OSSW_OK_MCU) \
    X(ST_BKU_1_1) X(ST_BKU_1_1v2) X(ST_BKU_1_2) X(ST_BKU_1_2v2) \
    X(ST_BKU_2_1) X(ST_BKU_2_1v2) X(ST_BKU_2_2) X(ST_BKU_2_2v2) \
    X(ST_BKU_3_1) X(ST_BKU_3_1v2) X(ST_BKU_3_2) X(ST_BKU_3_2v2) \
    X(ST_BKU_4_1) X(ST_BKU_4_1v2) X(ST_BKU_4_2) X(ST_BKU_4_2v2) \
    X(WO_TE_MCU)

#define PWM_CHANNEL_LIST(X) \
    X(ARK_PWM1_1) X(ARK_PWM1_2) X(ARK_PWM2_1) X(ARK_PWM2_2) \
    X(ARK_PWM3_1) X(ARK_PWM3_2) X(ARK_PWM4_1) X(ARK_PWM4_2) \
    X(ARK_PWM5_1) X(ARK_PWM5_2)

#define ADC_VOLTAGE_LIST(X) \
    X(ADC_VOLT_CH1, 350) X(ADC_VOLT_CH2, 700) X(ADC_VOLT_CH3, 1050) \
    X(ADC_VOLT_CH4, 1400) X(ADC_VOLT_CH5, 1750) X(ADC_VOLT_BUS, 2100) \
    X(CMAIN_BUS, 2450) X(MBUS_V, 2800) X(VBATP, 3150)

#define ADC_CURRENT_LIST(X) \
    X(ADC_CURR_CH1, 250) X(ADC_CURR_CH2, 500) X(ADC_CURR_CH3, 750) \
    X(ADC_CURR_CH4, 1000) X(ADC_CURR_CH5, 1250) X(BKU3_CUR, 1500) \
    X(BKU4_CUR, 1750) X(ADC_CURR_BAT, 2000) X(SPU_CUR, 2250) \
    X(BKU1_CUR, 2500) X(BKU2_CUR, 2750) X(BAT_CUR, 3000)

#define STRING_ITEM(name) #name,
#define ENUM_ITEM(name) {__COUNTER__ - enum_counter_base - 1, #name},
#define ADC_FIELD(name, base) KRUL_RESULT_I32("value_" #name, #name),

static const char* const output_pin_names[] = {OUTPUT_PIN_LIST(STRING_ITEM)};
static const char* const input_pin_names[] = {INPUT_PIN_LIST(STRING_ITEM)};
static const char* const pwm_channel_names[] = {PWM_CHANNEL_LIST(STRING_ITEM)};

enum { enum_counter_base = __COUNTER__ };
static const krul_enum_value_t all_pin_values[] = {
    INPUT_PIN_LIST(ENUM_ITEM) OUTPUT_PIN_LIST(ENUM_ITEM)};
#undef enum_counter_base
#define enum_counter_base output_enum_counter_base
enum { output_enum_counter_base = __COUNTER__ };
static const krul_enum_value_t output_pin_values[] = {
    OUTPUT_PIN_LIST(ENUM_ITEM)};
#undef enum_counter_base
#define enum_counter_base set_enum_counter_base
#define SET_ENUM_ITEM(name) {__COUNTER__ - enum_counter_base, #name},
enum { set_enum_counter_base = __COUNTER__ };
static const krul_enum_value_t pin_set_name_values[] = {
    {0, "All outputs"}, OUTPUT_PIN_LIST(SET_ENUM_ITEM)};
#undef SET_ENUM_ITEM
#undef enum_counter_base
static const krul_enum_value_t pin_type_values[] = {
    {0, "Input"}, {1, "Output"}};
static const krul_enum_value_t dac_values[] = {
    {0, "DACREF_MCU"}};
#define enum_counter_base pwm_enum_counter_base
enum { pwm_enum_counter_base = __COUNTER__ };
static const krul_enum_value_t pwm_values[] = {PWM_CHANNEL_LIST(ENUM_ITEM)};
#undef enum_counter_base
static const krul_enum_value_t fdcan_values[] = {
    {0, "FDCAN 1"}, {1, "FDCAN 2"}};
static const krul_enum_value_t uart_values[] = {{0, "UART 4 (TX)"}};
static const krul_enum_value_t memory_values[] = {
    {0, "Память STM32"},
    {1, "Внешняя FRAM"},
    {2, "Внешняя MRAM"}};

typedef struct {
    uint32_t base;
    uint32_t size;
    bool writable;
    uint8_t* data;
} memory_region_t;

struct ark_emulator_server {
    krul_server_t json_server;
    krul_server_t bson_server;
    krul_server_t cbor_server;
    serde_json_codec_t json_codec;
    serde_bson_codec_t bson_codec;
    serde_cbor_codec_t cbor_codec;
    serde_json_token_t tokens[SERDE_JSON_DEFAULT_TOKEN_COUNT];
    uint8_t json_response[ARK_EMULATOR_FRAME_SIZE];
    uint8_t bson_response[ARK_EMULATOR_FRAME_SIZE];
    uint8_t cbor_response[ARK_EMULATOR_FRAME_SIZE];
    bool output_states[ARRAY_SIZE(output_pin_names)];
    bool input_states[ARRAY_SIZE(input_pin_names)];
    uint32_t pwm_duty[ARRAY_SIZE(pwm_channel_names)];
    uint32_t pwm_period[ARRAY_SIZE(pwm_channel_names)];
    uint32_t dac_value;
    uint32_t adc_tick;
    uint32_t temperature_tick;
    int selected_qspi_bank;
    memory_region_t mcu_regions[6];
    uint8_t* fram;
    uint8_t* mram;
};

static ark_emulator_server_t* active_emulator;

static bool put_pin(krul_result_t* result, int32_t name, int32_t type,
                    bool state, bool include_type) {
    return krul_result_begin_object(result, NULL) &&
           krul_result_put_enum(result, "name", name) &&
           (!include_type || krul_result_put_enum(result, "type", type)) &&
           krul_result_put_i32(result, "state", state ? 1 : 0) &&
           krul_result_end_object(result);
}

static bool pin_get_handler(const krul_args_t* args, krul_result_t* result,
                            krul_error_t* error) {
    krul_array_t pins;
    (void)error;
    if (!krul_args_get_array(args, "pins", &pins) ||
        !krul_result_begin_array(result, "pins"))
        return false;
    size_t count = krul_array_get_size(&pins);
    if (count == 0U) {
        for (size_t i = 0U; i < ARRAY_SIZE(input_pin_names); ++i)
            if (!put_pin(result, (int32_t)i, 0,
                         active_emulator->input_states[i], true))
                return false;
        for (size_t i = 0U; i < ARRAY_SIZE(output_pin_names); ++i)
            if (!put_pin(result, (int32_t)(ARRAY_SIZE(input_pin_names) + i), 1,
                         active_emulator->output_states[i], true))
                return false;
    } else {
        for (size_t i = 0U; i < count; ++i) {
            int32_t code = 0;
            if (!krul_array_get_enum(&pins, i, &code)) return false;
            if (code >= 0 && (size_t)code < ARRAY_SIZE(input_pin_names)) {
                if (!put_pin(result, code, 0,
                             active_emulator->input_states[code], true))
                    return false;
                continue;
            }
            int index = (int)(code - (int32_t)ARRAY_SIZE(input_pin_names));
            if (index < 0 || (size_t)index >= ARRAY_SIZE(output_pin_names) ||
                !put_pin(result, code, 1,
                         active_emulator->output_states[index], true))
                return false;
        }
    }
    return krul_result_end_array(result) && krul_result_ok(result);
}

static bool pin_set_handler(const krul_args_t* args, krul_result_t* result,
                            krul_error_t* error) {
    krul_array_t pins;
    if (!krul_args_get_array(args, "pins", &pins)) return false;
    size_t count = krul_array_get_size(&pins);
    int indexes[ARRAY_SIZE(output_pin_names)];
    int32_t states[ARRAY_SIZE(output_pin_names)];
    bool set_all = false;
    for (size_t i = 0U; i < count; ++i) {
        krul_object_t pin;
        int32_t code = 0;
        if (!krul_array_get_object(&pins, i, &pin) ||
            !krul_object_get_enum(&pin, "name", &code) ||
            !krul_object_get_i32(&pin, "state", &states[i]))
            return false;
        if (count == 1U && code == 0) {
            set_all = true;
            continue;
        }
        indexes[i] = (int)code - 1;
        if (indexes[i] < 0) {
            krul_error_set(error, KRUL_ERROR_EXECUTION,
                           "Validated output pin code %ld is unavailable",
                           (long)code);
            return false;
        }
    }
    if (set_all) {
        for (size_t i = 0U; i < ARRAY_SIZE(output_pin_names); ++i)
            active_emulator->output_states[i] = states[0] != 0;
    } else {
        for (size_t i = 0U; i < count; ++i)
            active_emulator->output_states[indexes[i]] = states[i] != 0;
    }
    if (!krul_result_begin_array(result, "pins")) return false;
    if (set_all) {
        for (size_t i = 0U; i < ARRAY_SIZE(output_pin_names); ++i)
            if (!put_pin(result, (int32_t)i, 1,
                         active_emulator->output_states[i], false))
                return false;
    } else {
        for (size_t i = 0U; i < count; ++i)
            if (!put_pin(result, indexes[i], 1,
                         active_emulator->output_states[indexes[i]], false))
                return false;
    }
    return krul_result_end_array(result) && krul_result_ok(result);
}

static bool adc_read_handler(const krul_args_t* args, krul_result_t* result,
                             krul_error_t* error) {
    (void)args;
    (void)error;
    uint32_t tick = ++active_emulator->adc_tick;
#define PUT_ADC(name, base) \
    if (!krul_result_put_i32(result, "value_" #name, \
                             (int32_t)(((base) + tick) & 4095U))) return false;
    if (!krul_result_begin_object(result, "Voltage")) return false;
    ADC_VOLTAGE_LIST(PUT_ADC)
    if (!krul_result_end_object(result) ||
        !krul_result_begin_object(result, "Current"))
        return false;
    ADC_CURRENT_LIST(PUT_ADC)
#undef PUT_ADC
    return krul_result_end_object(result) && krul_result_ok(result);
}

static bool dac_set_handler(const krul_args_t* args, krul_result_t* result,
                            krul_error_t* error) {
    int32_t channel = 0;
    int32_t value;
    (void)error;
    if (!krul_args_get_enum(args, "channel", &channel) ||
        !krul_args_get_i32(args, "value", &value))
        return false;
    active_emulator->dac_value = (uint32_t)value;
    return krul_result_ok(result);
}

static bool dac_read_handler(const krul_args_t* args, krul_result_t* result,
                             krul_error_t* error) {
    int32_t channel = 0;
    (void)error;
    return krul_args_get_enum(args, "channel", &channel) &&
           krul_result_put_u32(result, "value", active_emulator->dac_value) &&
           krul_result_ok(result);
}

static int get_pwm(const krul_args_t* args) {
    int32_t channel = 0;
    if (!krul_args_get_enum(args, "channel", &channel)) return -1;
    return channel >= 0 && (size_t)channel < ARRAY_SIZE(pwm_channel_names)
               ? (int)channel : -1;
}

static bool pwm_set_handler(const krul_args_t* args, krul_result_t* result,
                            krul_error_t* error) {
    int32_t duty;
    int32_t period;
    int index = get_pwm(args);
    (void)error;
    if (index < 0 || !krul_args_get_i32(args, "duty_cycle", &duty) ||
        !krul_args_get_i32(args, "period_counter", &period))
        return false;
    active_emulator->pwm_duty[index] = (uint32_t)duty;
    active_emulator->pwm_period[index] = (uint32_t)period;
    return krul_result_ok(result);
}

static bool can_send_handler(const krul_args_t* args, krul_result_t* result,
                             krul_error_t* error) {
    char data[65];
    int32_t channel = 0;
    int32_t identifier;
    (void)error;
    if (!krul_args_get_enum(args, "channel", &channel) ||
        !krul_args_get_string(args, "data", data, sizeof(data)) ||
        !krul_args_get_i32(args, "id", &identifier))
        return false;
    (void)channel;
    (void)identifier;
    return krul_result_put_bool(result, "queued", true) &&
           krul_result_ok(result);
}

static bool uart_write_handler(const krul_args_t* args, krul_result_t* result,
                               krul_error_t* error) {
    int32_t destination = 0;
    char data[129];
    (void)error;
    return krul_args_get_enum(args, "destination", &destination) &&
           krul_args_get_string(args, "data", data, sizeof(data)) &&
           krul_result_put_u32(result, "bytes", (uint32_t)strlen(data)) &&
           krul_result_ok(result);
}

static uint8_t* region_data(memory_region_t* region) {
    if (region->data == NULL) region->data = (uint8_t*)calloc(region->size, 1U);
    return region->data;
}

static memory_region_t* find_mcu_region(uint32_t address, uint32_t size,
                                        bool writing) {
    for (size_t i = 0U; i < ARRAY_SIZE(active_emulator->mcu_regions); ++i) {
        memory_region_t* region = &active_emulator->mcu_regions[i];
        if ((!writing || region->writable) && address >= region->base &&
            address < region->base + region->size &&
            size <= region->base + region->size - address)
            return region;
    }
    return NULL;
}

static uint8_t* external_data(int32_t memory) {
    uint8_t** data = memory == 1
                         ? &active_emulator->fram : &active_emulator->mram;
    if (*data == NULL) *data = (uint8_t*)calloc(EXTERNAL_MEMORY_SIZE, 1U);
    return *data;
}

static bool mem_read_handler(const krul_args_t* args, krul_result_t* result,
                             krul_error_t* error) {
    int32_t memory = 0;
    int32_t address_value;
    int32_t size_value;
    if (!krul_args_get_enum(args, "memory", &memory) ||
        !krul_args_get_i32(args, "address", &address_value) ||
        !krul_args_get_i32(args, "size", &size_value))
        return false;
    uint32_t address = (uint32_t)address_value;
    uint32_t size = (uint32_t)size_value;
    uint8_t bytes[MEM_MAX_BUFFER] = {0};
    if (memory == 0) {
        memory_region_t* region = find_mcu_region(address, size, false);
        if (region == NULL) {
            krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                           "MCU memory range is not readable");
            return false;
        }
        uint8_t* data = region_data(region);
        if (data == NULL) return false;
        memcpy(bytes, data + address - region->base, size);
    } else {
        if (address >= EXTERNAL_MEMORY_SIZE || size > EXTERNAL_MEMORY_SIZE - address) {
            krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                           "External memory is unavailable or range is invalid");
            return false;
        }
        uint8_t* data = external_data(memory);
        if (data == NULL) return false;
        memcpy(bytes, data + address, size);
        active_emulator->selected_qspi_bank = memory == 1 ? 2 : 1;
    }
    char hex[MEM_MAX_BUFFER * 2U + 1U];
    for (size_t i = 0U; i < size; ++i)
        (void)snprintf(hex + i * 2U, sizeof(hex) - i * 2U, "%02x", bytes[i]);
    return krul_result_put_string(result, "data", hex) &&
           krul_result_ok(result);
}

static bool mem_write_handler(const krul_args_t* args, krul_result_t* result,
                              krul_error_t* error) {
    int32_t memory = 0;
    char data[MEM_MAX_BUFFER + 1U];
    int32_t address_value;
    if (!krul_args_get_enum(args, "memory", &memory) ||
        !krul_args_get_string(args, "data", data, sizeof(data)) ||
        !krul_args_get_i32(args, "address", &address_value))
        return false;
    uint32_t address = (uint32_t)address_value;
    uint32_t size = (uint32_t)strlen(data);
    uint8_t* destination;
    if (memory == 0) {
        memory_region_t* region = find_mcu_region(address, size, true);
        if (region == NULL) {
            krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                           "MCU memory range is not writable");
            return false;
        }
        destination = region_data(region);
        if (destination != NULL) destination += address - region->base;
    } else {
        if (address >= EXTERNAL_MEMORY_SIZE || size > EXTERNAL_MEMORY_SIZE - address) {
            krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                           "External memory is unavailable or range is invalid");
            return false;
        }
        destination = external_data(memory);
        if (destination != NULL) destination += address;
        active_emulator->selected_qspi_bank = memory == 1 ? 2 : 1;
    }
    if (destination == NULL) {
        krul_error_set(error, KRUL_ERROR_EXECUTION,
                       "Emulator memory allocation failed");
        return false;
    }
    memcpy(destination, data, size);
    return krul_result_put_u32(result, "bytes", size) &&
           krul_result_ok(result);
}

static bool qspi_status_handler(const krul_args_t* args, krul_result_t* result,
                                krul_error_t* error) {
    (void)args;
    (void)error;
    return krul_result_put_bool(result, "mram_ready", true) &&
           krul_result_put_bool(result, "fram_ready", true) &&
           krul_result_put_i32(result, "selected_bank",
                               active_emulator->selected_qspi_bank) &&
           krul_result_put_i32(result, "hal_state", 1) &&
           krul_result_put_string(result, "hal_error", "0x00000000") &&
           krul_result_ok(result);
}

static bool temperature_read_handler(const krul_args_t* args,
                                     krul_result_t* result,
                                     krul_error_t* error) {
    (void)args;
    (void)error;
    int32_t value = 22500 + (int32_t)(active_emulator->temperature_tick++ % 200U);
    return krul_result_put_f32(result, "temperature_C", (float)value / 1000.0F) &&
           krul_result_put_i32(result, "temperature_mC", value) &&
           krul_result_put_string(result, "bus", "1_3") &&
           krul_result_put_string(result, "power", "external") &&
           krul_result_put_string(result, "rom", "28A1B2C3D4E5F607") &&
           krul_result_ok(result);
}

static const krul_field_desc_t describe_params[] = {
    KRUL_STRING_REQUIRED("name", "Имя команды", 1, 47)};
static const krul_field_desc_t dac_set_params[] = {
    KRUL_ENUM_DEFAULT("channel", "Канал", dac_values, 0),
    {.name = "value", .label = "Значение", .type = KRUL_TYPE_I32,
     .widget = KRUL_WIDGET_SPECIAL_DAC, .has_default = true,
     .has_constraints = true, .default_value.i32 = 0,
     .constraints.i32 = {.min = 0, .max = 4095}}};
static const krul_field_desc_t dac_read_params[] = {
    KRUL_ENUM_DEFAULT("channel", "Канал", dac_values, 0)};
static const krul_field_desc_t pwm_params[] = {
    KRUL_ENUM_DEFAULT("channel", "Канал", pwm_values, 0),
    KRUL_I32_DEFAULT("duty_cycle", "Скважность, %", 0, 100, 0),
    KRUL_I32_DEFAULT("period_counter", "Период счётчика (такты таймера)",
                     1, 65535, 100)};
static const krul_field_desc_t can_send_params[] = {
    KRUL_ENUM_DEFAULT("channel", "Канал", fdcan_values, 0),
    KRUL_I32_DEFAULT("id", "Extended CAN ID", 0, 0x1FFFFFFF, 0x100),
    KRUL_STRING_REQUIRED("data", "Данные", 0, 64)};
static const krul_field_desc_t uart_params[] = {
    KRUL_ENUM_DEFAULT("destination", "Интерфейс", uart_values, 0),
    KRUL_STRING_REQUIRED("data", "Данные", 1, 128)};
static const krul_field_desc_t mem_read_params[] = {
    KRUL_ENUM_DEFAULT("memory", "Память", memory_values, 0),
    KRUL_I32_DEFAULT("address", "Адрес", 0, INT32_MAX, D2_SRAM_START),
    KRUL_I32_DEFAULT("size", "Размер", 1, MEM_MAX_BUFFER, 16)};
static const krul_field_desc_t mem_write_params[] = {
    KRUL_ENUM_DEFAULT("memory", "Память", memory_values, 0),
    KRUL_I32_DEFAULT("address", "Адрес", 0, INT32_MAX, D2_SRAM_START),
    KRUL_STRING_REQUIRED("data", "Данные", 1, MEM_MAX_BUFFER)};

static const krul_field_desc_t pin_get_item = {
    .type = KRUL_TYPE_ENUM, .has_constraints = true,
    .constraints.enumeration = {all_pin_values, (uint16_t)ARRAY_SIZE(all_pin_values)}};
static const krul_field_desc_t pin_get_params[] = {{
    .name = "pins", .label = "Выводы", .type = KRUL_TYPE_ARRAY,
    .has_default = true, .has_constraints = true,
    .schema.array = {&pin_get_item},
    .constraints.array = {0U, (uint16_t)ARRAY_SIZE(all_pin_values)}}};
static const krul_field_desc_t pin_get_result_fields[] = {
    {.name = "name", .type = KRUL_TYPE_ENUM, .has_constraints = true,
     .constraints.enumeration = {all_pin_values, (uint16_t)ARRAY_SIZE(all_pin_values)}},
    {.name = "type", .type = KRUL_TYPE_ENUM, .has_constraints = true,
     .constraints.enumeration = {pin_type_values, (uint16_t)ARRAY_SIZE(pin_type_values)}},
    KRUL_I32_REQUIRED("state", "Состояние", 0, 1)};
static const krul_field_desc_t pin_get_result_item = {
    .type = KRUL_TYPE_OBJECT,
    .schema.object = {pin_get_result_fields, (uint16_t)ARRAY_SIZE(pin_get_result_fields)}};
static const krul_field_desc_t pin_get_result[] = {{
    .name = "pins", .type = KRUL_TYPE_ARRAY, .has_constraints = true,
    .schema.array = {&pin_get_result_item},
    .constraints.array = {0U, (uint16_t)ARRAY_SIZE(all_pin_values)}}};

static const krul_field_desc_t pin_set_param_fields[] = {
    {.name = "name", .type = KRUL_TYPE_ENUM, .has_constraints = true,
     .constraints.enumeration = {pin_set_name_values, (uint16_t)ARRAY_SIZE(pin_set_name_values)}},
    KRUL_I32_REQUIRED("state", "Состояние", 0, 1)};
static const krul_field_desc_t pin_set_param_item = {
    .type = KRUL_TYPE_OBJECT,
    .schema.object = {pin_set_param_fields, (uint16_t)ARRAY_SIZE(pin_set_param_fields)}};
static const krul_field_desc_t pin_set_params[] = {{
    .name = "pins", .type = KRUL_TYPE_ARRAY, .has_constraints = true,
    .schema.array = {&pin_set_param_item},
    .constraints.array = {1U, (uint16_t)ARRAY_SIZE(output_pin_values)}}};
static const krul_field_desc_t pin_set_result_fields[] = {
    {.name = "name", .type = KRUL_TYPE_ENUM, .has_constraints = true,
     .constraints.enumeration = {output_pin_values, (uint16_t)ARRAY_SIZE(output_pin_values)}},
    KRUL_I32_REQUIRED("state", "Состояние", 0, 1)};
static const krul_field_desc_t pin_set_result_item = {
    .type = KRUL_TYPE_OBJECT,
    .schema.object = {pin_set_result_fields, (uint16_t)ARRAY_SIZE(pin_set_result_fields)}};
static const krul_field_desc_t pin_set_result[] = {{
    .name = "pins", .type = KRUL_TYPE_ARRAY, .has_constraints = true,
    .schema.array = {&pin_set_result_item},
    .constraints.array = {1U, (uint16_t)ARRAY_SIZE(output_pin_values)}}};

static const krul_field_desc_t adc_voltage_fields[] = {ADC_VOLTAGE_LIST(ADC_FIELD)};
static const krul_field_desc_t adc_current_fields[] = {ADC_CURRENT_LIST(ADC_FIELD)};
static const krul_field_desc_t adc_result[] = {
    {.name = "Voltage", .label = "Напряжение", .type = KRUL_TYPE_OBJECT,
     .widget = KRUL_WIDGET_SPECIAL_ADC_GROUP,
     .schema.object = {adc_voltage_fields, (uint16_t)ARRAY_SIZE(adc_voltage_fields)}},
    {.name = "Current", .label = "Ток", .type = KRUL_TYPE_OBJECT,
     .widget = KRUL_WIDGET_SPECIAL_ADC_GROUP,
     .schema.object = {adc_current_fields, (uint16_t)ARRAY_SIZE(adc_current_fields)}}};
static const krul_field_desc_t dac_read_result[] = {
    KRUL_RESULT_U32("value", "Значение")};
static const krul_field_desc_t can_send_result[] = {
    KRUL_RESULT_BOOL("queued", "Поставлено в очередь")};
static const krul_field_desc_t uart_result[] = {
    KRUL_RESULT_U32("bytes", "Передано байт")};
static const krul_field_desc_t mem_read_result[] = {
    KRUL_RESULT_STRING("data", "Данные (hex)", MEM_MAX_BUFFER * 2U)};
static const krul_field_desc_t mem_write_result[] = {
    KRUL_RESULT_U32("bytes", "Записано байт")};
static const krul_field_desc_t qspi_result[] = {
    KRUL_RESULT_BOOL("mram_ready", "MRAM работает"),
    KRUL_RESULT_BOOL("fram_ready", "FRAM работает"),
    KRUL_RESULT_I32("selected_bank", "Выбранный банк №"),
    KRUL_RESULT_I32("hal_state", "Состояние HAL"),
    KRUL_RESULT_STRING("hal_error", "Ошибка HAL", 16)};
static const krul_field_desc_t temperature_result[] = {
    KRUL_RESULT_F32("temperature_C", "Температура, °C"),
    KRUL_RESULT_I32("temperature_mC", "Температура, м°C"),
    KRUL_RESULT_STRING("bus", "Шина", 8),
    KRUL_RESULT_STRING("power", "Питание", 16),
    KRUL_RESULT_STRING("rom", "ROM", 16)};

#define COMMAND(name_, tab_, title_, group_, order_, params_, result_, handler_) \
    {.name = name_, .type = KRUL_CMD_NORMAL, .tab = tab_, .title = title_, \
     .group = group_, .order = order_, .params = params_, \
     .params_count = ARRAY_SIZE(params_), .result = result_, \
     .result_count = ARRAY_SIZE(result_), .handler = handler_}

static const krul_command_t cmd_whoami = {.name = "WHOAMI", .type = KRUL_CMD_BUILTIN};
static const krul_command_t cmd_ping = {.name = "PING", .type = KRUL_CMD_BUILTIN};
static const krul_command_t cmd_list = {.name = "CMD_LIST", .type = KRUL_CMD_BUILTIN};
static const krul_command_t cmd_describe = {
    .name = "DESCRIBE", .type = KRUL_CMD_BUILTIN, .params = describe_params,
    .params_count = ARRAY_SIZE(describe_params)};
static const krul_command_t cmd_pin_get = {
    .name = "PIN_GET", .type = KRUL_CMD_NORMAL, .widget = KRUL_WIDGET_SPECIAL_GPIO,
    .params = pin_get_params, .params_count = ARRAY_SIZE(pin_get_params),
    .result = pin_get_result, .result_count = ARRAY_SIZE(pin_get_result),
    .handler = pin_get_handler};
static const krul_command_t cmd_pin_set = {
    .name = "PIN_SET", .type = KRUL_CMD_NORMAL, .widget = KRUL_WIDGET_SPECIAL_GPIO,
    .params = pin_set_params, .params_count = ARRAY_SIZE(pin_set_params),
    .result = pin_set_result, .result_count = ARRAY_SIZE(pin_set_result),
    .handler = pin_set_handler};
static const krul_command_t cmd_adc_read = {
    .name = "ADC_READ", .type = KRUL_CMD_NORMAL, .tab = "Аналоговые сигналы",
    .title = "Мониторинг АЦП", .group = "АЦП", .order = 10,
    .result = adc_result, .result_count = ARRAY_SIZE(adc_result), .autoupdate = true,
    .min_period_ms = 100, .max_period_ms = 1000, .default_period_ms = 500,
    .handler = adc_read_handler};
static const krul_command_t cmd_dac_set = {
    .name = "DAC_SET", .type = KRUL_CMD_NORMAL, .tab = "Аналоговые сигналы",
    .title = "Установить ЦАП", .group = "ЦАП", .order = 20,
    .params = dac_set_params, .params_count = ARRAY_SIZE(dac_set_params),
    .handler = dac_set_handler};
static const krul_command_t cmd_dac_read = {
    .name = "DAC_READ", .type = KRUL_CMD_NOGUI, .tab = "Аналоговые сигналы",
    .title = "Прочитать ЦАП", .group = "ЦАП", .order = 30,
    .params = dac_read_params, .params_count = ARRAY_SIZE(dac_read_params),
    .result = dac_read_result, .result_count = ARRAY_SIZE(dac_read_result),
    .autoupdate = true, .min_period_ms = 100, .max_period_ms = 5000,
    .default_period_ms = 500, .handler = dac_read_handler};
static const krul_command_t cmd_pwm_set = {
    .name = "PWM_SET", .type = KRUL_CMD_NORMAL,
    .widget = KRUL_WIDGET_SPECIAL_PWM, .tab = "PWM",
    .title = "Управление ШИМ", .group = "PWM", .order = 10,
    .params = pwm_params, .params_count = ARRAY_SIZE(pwm_params),
    .handler = pwm_set_handler};
static const krul_command_t cmd_can_send = COMMAND(
    "CAN_SEND", "Интерфейсы", "Отправить CAN FD", "CAN FD", 10,
    can_send_params, can_send_result, can_send_handler);
static const krul_command_t cmd_uart_write = COMMAND(
    "UART_WRITE", "Интерфейсы", "Отправить UART", "UART", 30,
    uart_params, uart_result, uart_write_handler);
static const krul_command_t cmd_mem_read = COMMAND(
    "MEM_READ", "Диагностика", "Прочитать память", "Память", 10,
    mem_read_params, mem_read_result, mem_read_handler);
static const krul_command_t cmd_mem_write = COMMAND(
    "MEM_WRITE", "Диагностика", "Записать память", "Память", 20,
    mem_write_params, mem_write_result, mem_write_handler);
static const krul_command_t cmd_qspi_status = {
    .name = "QSPI_STATUS", .type = KRUL_CMD_NORMAL, .tab = "Диагностика",
    .title = "Состояние QSPI", .group = "Память", .order = 30,
    .result = qspi_result, .result_count = ARRAY_SIZE(qspi_result),
    .handler = qspi_status_handler};
static const krul_command_t cmd_temperature = {
    .name = "TEMPERATURE_READ", .type = KRUL_CMD_NORMAL, .tab = "Датчики",
    .title = "Температура DS18S20", .group = "Температура", .order = 10,
    .result = temperature_result, .result_count = ARRAY_SIZE(temperature_result),
    .autoupdate = true, .min_period_ms = 1000, .max_period_ms = 30000,
    .default_period_ms = 2000, .handler = temperature_read_handler};

static const krul_command_t* const commands[] = {
    &cmd_ping, &cmd_whoami, &cmd_list, &cmd_describe, &cmd_pin_get, &cmd_pin_set,
    &cmd_adc_read, &cmd_dac_set, &cmd_dac_read, &cmd_pwm_set,
    &cmd_can_send, &cmd_uart_write, &cmd_mem_read,
    &cmd_mem_write, &cmd_qspi_status, &cmd_temperature};

ark_emulator_server_t* ark_emulator_server_create(void) {
    if (active_emulator != NULL) return NULL;
    ark_emulator_server_t* emulator =
        (ark_emulator_server_t*)calloc(1U, sizeof(*emulator));
    if (emulator == NULL) return NULL;
    emulator->selected_qspi_bank = 1;
    for (size_t i = 0U; i < ARRAY_SIZE(emulator->pwm_period); ++i)
        emulator->pwm_period[i] = 100U;
    emulator->mcu_regions[0] = (memory_region_t){FLASH_START, FLASH_SIZE, false, NULL};
    emulator->mcu_regions[1] = (memory_region_t){ITCM_START, ITCM_SIZE, true, NULL};
    emulator->mcu_regions[2] = (memory_region_t){DTCM_START, DTCM_SIZE, true, NULL};
    emulator->mcu_regions[3] = (memory_region_t){AXI_SRAM_START, AXI_SRAM_SIZE, true, NULL};
    emulator->mcu_regions[4] = (memory_region_t){D2_SRAM_START, D2_SRAM_SIZE, true, NULL};
    emulator->mcu_regions[5] = (memory_region_t){D3_SRAM_START, D3_SRAM_SIZE, true, NULL};
    if (!serde_json_init(&emulator->json_codec, emulator->tokens,
                         ARRAY_SIZE(emulator->tokens)) ||
        !serde_bson_init(&emulator->bson_codec) ||
        !serde_cbor_init(&emulator->cbor_codec)) {
        free(emulator);
        return NULL;
    }
    const krul_server_config_t config = {
        .commands = commands, .command_count = ARRAY_SIZE(commands),
        .device_name = "АРК", .firmware_version = "1.0.0", .protocol_version = 4U,
        .codec = serde_json(&emulator->json_codec),
        .response_buffer = emulator->json_response,
        .response_capacity = ARK_EMULATOR_FRAME_SIZE};
    krul_server_config_t bson_config = config;
    bson_config.codec = serde_bson(&emulator->bson_codec);
    bson_config.response_buffer = emulator->bson_response;
    krul_server_config_t cbor_config = config;
    cbor_config.codec = serde_cbor(&emulator->cbor_codec);
    cbor_config.response_buffer = emulator->cbor_response;
    if (!krul_server_init(&emulator->json_server, &config) ||
        !krul_server_init(&emulator->bson_server, &bson_config) ||
        !krul_server_init(&emulator->cbor_server, &cbor_config)) {
        free(emulator);
        return NULL;
    }
    active_emulator = emulator;
    return emulator;
}

void ark_emulator_server_destroy(ark_emulator_server_t* emulator) {
    if (emulator == NULL) return;
    for (size_t i = 0U; i < ARRAY_SIZE(emulator->mcu_regions); ++i)
        free(emulator->mcu_regions[i].data);
    free(emulator->fram);
    free(emulator->mram);
    if (active_emulator == emulator) active_emulator = NULL;
    free(emulator);
}

krul_server_t* ark_emulator_server_krul(ark_emulator_server_t* emulator) {
    return emulator == NULL ? NULL : &emulator->json_server;
}

krul_server_t* ark_emulator_server_krul_for_format(
    ark_emulator_server_t* emulator, krul_transport_format_t format) {
    if (emulator == NULL) return NULL;
    if (format == KRUL_TRANSPORT_FORMAT_JSON) return &emulator->json_server;
    if (format == KRUL_TRANSPORT_FORMAT_BSON) return &emulator->bson_server;
    if (format == KRUL_TRANSPORT_FORMAT_CBOR) return &emulator->cbor_server;
    return NULL;
}
