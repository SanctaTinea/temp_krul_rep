#include "krul_internal.h"

/*
 * Runtime-слой входных значений.
 *
 * Первая часть рекурсивно сверяет декодированные узлы serde со схемой params. Вторая
 * предоставляет обработчику безопасные семейства функций krul_args, krul_array и
 * krul_object, чтобы прикладной код не зависел от JSON и не повторял defaults.
 */

#include <string.h>

static bool key_matches_field(const serde_key_view_t* view,
                              const krul_field_desc_t* field) {
    if (view->has_tag && view->tag == krul_field_tag(field))
        return true;
    if (!view->has_name || field->name == NULL) return false;
    size_t length = strlen(field->name);
    return view->name_length == length &&
           memcmp(view->name, field->name, length) == 0;
}

static const krul_field_desc_t* find_field_view(
    const krul_field_desc_t* fields, uint16_t count,
    const serde_key_view_t* key, uint16_t* found_index) {
    for (uint16_t index = 0U; index < count; ++index) {
        if (key_matches_field(key, &fields[index])) {
            if (found_index != NULL) *found_index = index;
            return &fields[index];
        }
    }
    return NULL;
}

static bool validate_enum_value(serde_codec_t codec, serde_node_t node,
                                const krul_field_desc_t* field,
                                krul_error_t* error) {
    int32_t value = 0;
    if (!serde_get_i32(codec, node, &value)) {
        krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                       "Field '%s' must be an enum integer",
                       krul_field_name(field));
        return false;
    }
    for (uint16_t index = 0U; index < field->constraints.enumeration.count;
         ++index) {
        if (value == field->constraints.enumeration.values[index].value)
            return true;
    }
    krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                   "Field '%s' has an unknown enum value",
                   krul_field_name(field));
    return false;
}

static bool validate_input_value(serde_codec_t codec, serde_node_t node,
                                 const krul_field_desc_t* field,
                                 krul_error_t* error);

bool krul_validate_input_object(serde_codec_t codec, serde_node_t object,
                                const krul_field_desc_t* fields,
                                uint16_t field_count, krul_error_t* error) {
    if (serde_kind(codec, object) != SERDE_KIND_OBJECT) {
        krul_error_set(error, KRUL_ERROR_INVALID_TYPE, "Expected object");
        return false;
    }
    /* Неизвестные поля пропускаются для прямой совместимости протокола.
     * Известные дубликаты запрещены и отмечаются битовой маской. */
    uint64_t seen = 0U;
    size_t member_count = serde_object_size(codec, object);
    for (size_t member = 0U; member < member_count; ++member) {
        serde_key_view_t key = {0};
        serde_node_t value = {0};
        if (!serde_object_member(codec, object, member, &key, &value)) {
            krul_error_set(error, KRUL_ERROR_INVALID_REQUEST,
                           "Invalid object member");
            return false;
        }
        uint16_t field_index = 0U;
        const krul_field_desc_t* field =
            find_field_view(fields, field_count, &key, &field_index);
        if (field == NULL) continue;
        uint64_t bit = UINT64_C(1) << field_index;
        if ((seen & bit) != 0U) {
            krul_error_set(error, KRUL_ERROR_INVALID_REQUEST,
                           "Duplicate field '%s'", field->name);
            return false;
        }
        seen |= bit;
        if (!validate_input_value(codec, value, field, error)) return false;
    }
    for (uint16_t index = 0U; index < field_count; ++index) {
        if ((seen & (UINT64_C(1) << index)) == 0U &&
            !fields[index].has_default) {
            krul_error_set(error, KRUL_ERROR_MISSING_FIELD,
                           "Missing field '%s'", fields[index].name);
            return false;
        }
    }
    return true;
}

bool krul_user_validator_get_i32(const krul_value_ref_t* value, int32_t* output) {
    if (value == NULL || output == NULL || value->desc == NULL ||
        value->desc->type != KRUL_TYPE_I32)
        return false;
    /* direct используется для пользовательской проверки default/result; иначе
     * читаем значение из узла serde входного кадра. */
    if (value->direct) {
        *output = value->value.i32;
        return true;
    }
    return value->present && serde_get_i32(value->codec, value->node, output);
}

bool krul_user_validator_get_u32(const krul_value_ref_t* value, uint32_t* output) {
    if (value == NULL || output == NULL || value->desc == NULL ||
        value->desc->type != KRUL_TYPE_U32)
        return false;
    if (value->direct) {
        *output = value->value.u32;
        return true;
    }
    return value->present && serde_get_u32(value->codec, value->node, output);
}

bool krul_user_validator_get_f32(const krul_value_ref_t* value, float* output) {
    if (value == NULL || output == NULL || value->desc == NULL ||
        value->desc->type != KRUL_TYPE_F32)
        return false;
    if (value->direct) {
        *output = value->value.f32;
        return true;
    }
    return value->present && serde_get_f32(value->codec, value->node, output);
}

bool krul_user_validator_get_bool(const krul_value_ref_t* value, bool* output) {
    if (value == NULL || output == NULL || value->desc == NULL ||
        value->desc->type != KRUL_TYPE_BOOL)
        return false;
    if (value->direct) {
        *output = value->value.boolean;
        return true;
    }
    return value->present && serde_get_bool(value->codec, value->node, output);
}

bool krul_user_validator_get_enum(const krul_value_ref_t* value,
                                  int32_t* output) {
    if (value == NULL || output == NULL || value->desc == NULL ||
        value->desc->type != KRUL_TYPE_ENUM)
        return false;
    if (value->direct) {
        *output = value->value.i32;
        return true;
    }
    return value->present && serde_get_i32(value->codec, value->node, output);
}

bool krul_user_validator_get_string(const krul_value_ref_t* value, char* output,
                           size_t capacity) {
    if (value == NULL || output == NULL || capacity == 0U ||
        value->desc == NULL ||
        (value->desc->type != KRUL_TYPE_STRING &&
         value->desc->type != KRUL_TYPE_CONSOLE_STRING))
        return false;
    if (value->direct) {
        if (value->value.string == NULL ||
            strlen(value->value.string) >= capacity)
            return false;
        memcpy(output, value->value.string, strlen(value->value.string) + 1U);
        return true;
    }
    return value->present &&
           serde_get_string(value->codec, value->node, output, capacity, NULL);
}

bool krul_user_validator_get_count(const krul_value_ref_t* value, size_t* output) {
    if (value == NULL || output == NULL || value->desc == NULL ||
        (value->desc->type != KRUL_TYPE_ARRAY &&
         value->desc->type != KRUL_TYPE_OBJECT))
        return false;
    if (value->direct) {
        *output = value->value.count;
        return true;
    }
    if (!value->present) return false;
    *output = value->desc->type == KRUL_TYPE_ARRAY
                  ? serde_array_size(value->codec, value->node)
                  : serde_object_size(value->codec, value->node);
    return true;
}

static bool object_field_node(serde_codec_t codec, serde_node_t object,
                              const krul_field_desc_t* field,
                              serde_node_t* node) {
    serde_key_t key = krul_field_key(field);
    size_t matches = 0U;
    return serde_object_get(codec, object, &key, node, &matches) &&
           matches == 1U;
}

static const krul_field_desc_t* args_field(const krul_args_t* args,
                                           const char* name,
                                           serde_node_t* node,
                                           bool* present) {
    /* Возвращаем дескриптор даже при отсутствии поля: типизированное чтение само выберет
     * default или сообщит false для обязательного значения. */
    if (args == NULL) return NULL;
    const krul_field_desc_t* field = krul_find_field(
        args->command->params, args->command->params_count, name, NULL);
    if (field == NULL) return NULL;
    *present = args->params_present &&
               object_field_node(args->codec, args->params, field, node);
    return field;
}

bool krul_args_get_i32(const krul_args_t* args, const char* name, int32_t* value) {
    serde_node_t node = {0};
    bool present = false;
    const krul_field_desc_t* field = args_field(args, name, &node, &present);
    if (field == NULL || field->type != KRUL_TYPE_I32 || value == NULL)
        return false;
    if (!present) {
        if (!field->has_default) return false;
        *value = field->default_value.i32;
        return true;
    }
    return serde_get_i32(args->codec, node, value);
}

bool krul_args_get_u32(const krul_args_t* args, const char* name, uint32_t* value) {
    serde_node_t node = {0};
    bool present = false;
    const krul_field_desc_t* field = args_field(args, name, &node, &present);
    if (field == NULL || field->type != KRUL_TYPE_U32 || value == NULL)
        return false;
    if (!present) {
        if (!field->has_default) return false;
        *value = field->default_value.u32;
        return true;
    }
    return serde_get_u32(args->codec, node, value);
}

bool krul_args_get_f32(const krul_args_t* args, const char* name, float* value) {
    serde_node_t node = {0};
    bool present = false;
    const krul_field_desc_t* field = args_field(args, name, &node, &present);
    if (field == NULL || field->type != KRUL_TYPE_F32 || value == NULL)
        return false;
    if (!present) {
        if (!field->has_default) return false;
        *value = field->default_value.f32;
        return true;
    }
    return serde_get_f32(args->codec, node, value);
}

bool krul_args_get_bool(const krul_args_t* args, const char* name, bool* value) {
    serde_node_t node = {0};
    bool present = false;
    const krul_field_desc_t* field = args_field(args, name, &node, &present);
    if (field == NULL || field->type != KRUL_TYPE_BOOL || value == NULL)
        return false;
    if (!present) {
        if (!field->has_default) return false;
        *value = field->default_value.boolean;
        return true;
    }
    return serde_get_bool(args->codec, node, value);
}

bool krul_args_get_enum(const krul_args_t* args, const char* name,
                        int32_t* value) {
    serde_node_t node = {0};
    bool present = false;
    const krul_field_desc_t* field = args_field(args, name, &node, &present);
    if (field == NULL || field->type != KRUL_TYPE_ENUM || value == NULL)
        return false;
    if (!present) {
        if (!field->has_default) return false;
        *value = field->default_value.i32;
        return true;
    }
    return serde_get_i32(args->codec, node, value);
}

bool krul_args_get_string(const krul_args_t* args, const char* name, char* output,
                      size_t capacity) {
    serde_node_t node = {0};
    bool present = false;
    const krul_field_desc_t* field = args_field(args, name, &node, &present);
    if (field == NULL ||
        (field->type != KRUL_TYPE_STRING &&
         field->type != KRUL_TYPE_CONSOLE_STRING) ||
        output == NULL || capacity == 0U)
        return false;
    if (present)
        return serde_get_string(args->codec, node, output, capacity, NULL);
    const char* default_value = field->default_value.string;
    if (!field->has_default || default_value == NULL ||
        strlen(default_value) >= capacity)
        return false;
    memcpy(output, default_value, strlen(default_value) + 1U);
    return true;
}

bool krul_args_get_array(const krul_args_t* args, const char* name,
                     krul_array_t* array) {
    /* krul_array_t хранит не копию элементов, а «окно» над исходным деревом
       serde и схему одного элемента. Поэтому handle действителен только пока
       живёт разобранный запрос. */
    serde_node_t node = {0};
    bool present = false;
    const krul_field_desc_t* field = args_field(args, name, &node, &present);
    if (field == NULL || field->type != KRUL_TYPE_ARRAY || array == NULL)
        return false;
    *array = (krul_array_t){.codec = args->codec,
                            .node = node,
                            .element_desc = field->schema.array.element,
                            .present = present};
    return present || field->has_default;
}

size_t krul_array_get_size(const krul_array_t* array) {
    /* У отсутствующего массива с default нет materialized-элементов: его
       логическое значение — пустой массив. */
    return array != NULL && array->present
               ? serde_array_size(array->codec, array->node)
               : 0U;
}

bool krul_array_get_string(const krul_array_t* array, size_t index, char* output,
                       size_t capacity) {
    if (array == NULL || !array->present || array->element_desc == NULL ||
        array->element_desc->type != KRUL_TYPE_STRING)
        return false;
    serde_node_t node = {0};
    return serde_array_get(array->codec, array->node, index, &node) &&
           serde_get_string(array->codec, node, output, capacity, NULL);
}

bool krul_array_get_enum(const krul_array_t* array, size_t index,
                         int32_t* value) {
    if (array == NULL || !array->present || array->element_desc == NULL ||
        array->element_desc->type != KRUL_TYPE_ENUM || value == NULL)
        return false;
    serde_node_t node = {0};
    return serde_array_get(array->codec, array->node, index, &node) &&
           serde_get_i32(array->codec, node, value);
}

bool krul_array_get_object(const krul_array_t* array, size_t index,
                       krul_object_t* object) {
    if (array == NULL || !array->present || object == NULL ||
        array->element_desc == NULL ||
        array->element_desc->type != KRUL_TYPE_OBJECT)
        return false;
    serde_node_t node = {0};
    if (!serde_array_get(array->codec, array->node, index, &node)) return false;
    *object = (krul_object_t){
        .codec = array->codec,
        .node = node,
        .fields = array->element_desc->schema.object.fields,
        .field_count = array->element_desc->schema.object.count};
    return true;
}

static const krul_field_desc_t* object_field(const krul_object_t* object,
                                             const char* name,
                                             serde_node_t* node) {
    /* Доступ к вложенному объекту учитывает схему: приложение не может
       случайно прочитать поле, которого нет в описании команды. */
    if (object == NULL) return NULL;
    const krul_field_desc_t* field =
        krul_find_field(object->fields, object->field_count, name, NULL);
    return field != NULL &&
                   object_field_node(object->codec, object->node, field, node)
               ? field
               : NULL;
}

bool krul_object_get_i32(const krul_object_t* object, const char* name,
                     int32_t* value) {
    serde_node_t node = {0};
    const krul_field_desc_t* field = object_field(object, name, &node);
    return field != NULL && field->type == KRUL_TYPE_I32 && value != NULL &&
           serde_get_i32(object->codec, node, value);
}

bool krul_object_get_u32(const krul_object_t* object, const char* name,
                     uint32_t* value) {
    serde_node_t node = {0};
    const krul_field_desc_t* field = object_field(object, name, &node);
    return field != NULL && field->type == KRUL_TYPE_U32 && value != NULL &&
           serde_get_u32(object->codec, node, value);
}

bool krul_object_get_bool(const krul_object_t* object, const char* name,
                      bool* value) {
    serde_node_t node = {0};
    const krul_field_desc_t* field = object_field(object, name, &node);
    return field != NULL && field->type == KRUL_TYPE_BOOL && value != NULL &&
           serde_get_bool(object->codec, node, value);
}

bool krul_object_get_enum(const krul_object_t* object, const char* name,
                          int32_t* value) {
    serde_node_t node = {0};
    const krul_field_desc_t* field = object_field(object, name, &node);
    return field != NULL && field->type == KRUL_TYPE_ENUM && value != NULL &&
           serde_get_i32(object->codec, node, value);
}

bool krul_object_get_string(const krul_object_t* object, const char* name,
                        char* output, size_t capacity) {
    serde_node_t node = {0};
    const krul_field_desc_t* field = object_field(object, name, &node);
    return field != NULL &&
           field->type == KRUL_TYPE_STRING &&
           serde_get_string(object->codec, node, output, capacity, NULL);
}

static bool validate_input_value(serde_codec_t codec, serde_node_t node,
                                 const krul_field_desc_t* field,
                                 krul_error_t* error) {
    /* Здесь сходятся три уровня проверки одного значения:
       1) представление wire-формата можно преобразовать в требуемый тип;
       2) значение удовлетворяет декларативным ограничениям дескриптора;
       3) прикладная функция обратного вызова validate принимает семантически
          корректное поле. */
    int32_t i32 = 0;
    uint32_t u32 = 0U;
    float f32 = 0.0F;
    bool boolean = false;
    size_t length = 0U;
    switch (field->type) {
        case KRUL_TYPE_I32:
            if (!serde_get_i32(codec, node, &i32)) {
                krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                               "Field '%s' must be int32",
                               krul_field_name(field));
                return false;
            }
            if (field->has_constraints &&
                (i32 < field->constraints.i32.min ||
                 i32 > field->constraints.i32.max)) {
                krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                               "Field '%s' must be %ld..%ld",
                               krul_field_name(field),
                               (long)field->constraints.i32.min,
                               (long)field->constraints.i32.max);
                return false;
            }
            break;
        case KRUL_TYPE_U32:
            if (!serde_get_u32(codec, node, &u32)) {
                krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                               "Field '%s' must be uint32",
                               krul_field_name(field));
                return false;
            }
            if (field->has_constraints &&
                (u32 < field->constraints.u32.min ||
                 u32 > field->constraints.u32.max)) {
                krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                               "Field '%s' is outside its uint32 range",
                               krul_field_name(field));
                return false;
            }
            break;
        case KRUL_TYPE_F32:
            if (!serde_get_f32(codec, node, &f32)) {
                krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                               "Field '%s' must be a finite number",
                               krul_field_name(field));
                return false;
            }
            if (field->has_constraints &&
                (f32 < field->constraints.f32.min ||
                 f32 > field->constraints.f32.max)) {
                krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                               "Field '%s' is outside its numeric range",
                               krul_field_name(field));
                return false;
            }
            break;
        case KRUL_TYPE_BOOL:
            if (!serde_get_bool(codec, node, &boolean)) {
                krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                               "Field '%s' must be boolean",
                               krul_field_name(field));
                return false;
            }
            break;
        case KRUL_TYPE_STRING:
        case KRUL_TYPE_CONSOLE_STRING:
            if (!serde_get_string(codec, node, NULL, 0U, &length)) {
                krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                               "Field '%s' must be string",
                               krul_field_name(field));
                return false;
            }
            if (field->has_constraints) {
                uint16_t minimum = field->type == KRUL_TYPE_STRING
                                       ? field->constraints.string.min_length
                                       : 0U;
                uint16_t maximum = field->type == KRUL_TYPE_STRING
                                       ? field->constraints.string.max_length
                                       : field->constraints.console.max_length;
                if (length < minimum || length > maximum) {
                    krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                                   "Field '%s' has invalid string length",
                                   krul_field_name(field));
                    return false;
                }
            }
            break;
        case KRUL_TYPE_ENUM:
            if (!validate_enum_value(codec, node, field, error)) return false;
            break;
        case KRUL_TYPE_ARRAY: {
            if (serde_kind(codec, node) != SERDE_KIND_ARRAY) {
                krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                               "Field '%s' must be array",
                               krul_field_name(field));
                return false;
            }
            size_t count = serde_array_size(codec, node);
            if (field->has_constraints &&
                (count < field->constraints.array.min_count ||
                 count > field->constraints.array.max_count)) {
                krul_error_set(error, KRUL_ERROR_OUT_OF_RANGE,
                               "Field '%s' has invalid array length",
                               krul_field_name(field));
                return false;
            }
            for (size_t index = 0U; index < count; ++index) {
                /* Одна и та же функция рекурсивно проверяет массивы объектов и
                   другие вложенные комбинации без специальных веток. */
                serde_node_t item = {0};
                if (!serde_array_get(codec, node, index, &item) ||
                    !validate_input_value(codec, item,
                                          field->schema.array.element, error))
                    return false;
            }
            break;
        }
        case KRUL_TYPE_OBJECT:
            if (!krul_validate_input_object(codec, node,
                                            field->schema.object.fields,
                                            field->schema.object.count, error))
                return false;
            break;
        default:
            krul_error_set(error, KRUL_ERROR_INVALID_TYPE,
                           "Unsupported type for field '%s'",
                           krul_field_name(field));
            return false;
    }
    if (field->validate != NULL) {
        /* Пользовательская проверка запускается последней: функция обратного вызова уже может безопасно
           читать значение через семейство krul_value_*(). */
        krul_value_ref_t value = {.codec = codec,
                                  .node = node,
                                  .desc = field,
                                  .present = true};
        if (!field->validate(&value, error, field->validate_context))
            return false;
    }
    return true;
}
