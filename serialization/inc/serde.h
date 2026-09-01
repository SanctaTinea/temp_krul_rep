#pragma once

/**
 * @file serde.h
 * @brief Не зависящий от формата API чтения и записи без динамической памяти.
 *
 * Кодек представлен таблицей методов и принадлежащим реализации указателем
 * `self`. Декодированные узлы заимствуют память кодека и входных данных;
 * средства записи используют фиксированную память вызывающей стороны и ограниченный
 * выходной буфер.
 *
 * @defgroup serde Интерфейс сериализации
 * @brief Не зависящее от формата декодирование и потоковое кодирование.
 * @{
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Максимальный объём внутренней памяти реализации средства записи кодека. */
#define SERDE_WRITER_STORAGE_SIZE 192U

/** Состояние, возвращаемое операциями декодирования кодека. */
typedef enum {
    SERDE_OK = 0,             /**< Операция успешно завершена. */
    SERDE_ERROR_MALFORMED,    /**< Входные данные нарушают синтаксис формата. */
    SERDE_ERROR_NO_SPACE,     /**< Память токенов или вывода исчерпана. */
    SERDE_ERROR_TYPE,         /**< Узел нельзя прочитать как запрошенный тип. */
    SERDE_ERROR_STATE,        /**< Кодек или средство записи недействительны либо неверно используются. */
    SERDE_ERROR_UNSUPPORTED   /**< Операция не поддерживается этим кодеком. */
} serde_status_t;

/** Не зависящий от формата вид декодированного значения. */
typedef enum {
    SERDE_KIND_INVALID = 0,
    SERDE_KIND_NULL,
    SERDE_KIND_OBJECT,
    SERDE_KIND_ARRAY,
    SERDE_KIND_STRING,
    SERDE_KIND_NUMBER,
    SERDE_KIND_BOOL
} serde_kind_t;

/** Ключ, передаваемый средству записи или поиску в объекте. */
typedef struct {
    const char* name; /**< Текстовый ключ или NULL для кодека только с тегами. */
    uint16_t tag;     /**< Числовой ключ компактного кодека; ноль, если не используется. */
} serde_key_t;

/** Заимствованное представление ключа объекта, возвращённое кодеком. */
typedef struct {
    const char* name;   /**< Заимствованные байты имени без терминатора. */
    size_t name_length; /**< Число байтов имени. */
    uint16_t tag;       /**< Числовой тег, если `has_tag` равен true. */
    bool has_name;      /**< Присутствует ли текстовое имя. */
    bool has_tag;       /**< Присутствует ли числовой тег. */
} serde_key_view_t;

/**
 * Stable compact tag for a UTF-8 key name.  Explicit schema tags may override
 * it; the derived value is never zero and is shared by Krul CBOR clients.
 */
uint16_t serde_key_tag(const char* name);

/** Непрозрачный дескриптор декодированного узла для исходного кодека. */
typedef struct {
    uint32_t value;
} serde_node_t;

/** Выровненная память вызывающей стороны для потокового средства записи. */
typedef union {
#ifdef _MSC_VER
    long double scalar_alignment;
    void* pointer_alignment;
#else
    max_align_t alignment;
#endif
    uint8_t bytes[SERDE_WRITER_STORAGE_SIZE];
} serde_writer_storage_t;

/** Таблица методов внутренней реализации потокового средства записи. */
typedef struct serde_writer_mtab {
    /** @brief Открыть объект. @param self Состояние реализации. @param key Контекстный ключ. @return Признак успеха. */
    bool (*begin_object)(void* self, const serde_key_t* key);
    /** @brief Закрыть текущий объект. @param self Состояние реализации. @return Признак успеха. */
    bool (*end_object)(void* self);
    /** @brief Открыть массив. @param self Состояние реализации. @param key Контекстный ключ. @return Признак успеха. */
    bool (*begin_array)(void* self, const serde_key_t* key);
    /** @brief Закрыть текущий массив. @param self Состояние реализации. @return Признак успеха. */
    bool (*end_array)(void* self);
    /** @brief Закодировать знаковое целое число. @param self Состояние реализации. @param key Контекстный ключ. @param value Значение. @return Признак успеха. */
    bool (*put_i32)(void* self, const serde_key_t* key, int32_t value);
    /** @brief Закодировать беззнаковое целое число. @param self Состояние реализации. @param key Контекстный ключ. @param value Значение. @return Признак успеха. */
    bool (*put_u32)(void* self, const serde_key_t* key, uint32_t value);
    /** @brief Закодировать конечное число с плавающей точкой. @param self Состояние реализации. @param key Контекстный ключ. @param value Значение. @return Признак успеха. */
    bool (*put_f32)(void* self, const serde_key_t* key, float value);
    /** @brief Закодировать логическое значение. @param self Состояние реализации. @param key Контекстный ключ. @param value Значение. @return Признак успеха. */
    bool (*put_bool)(void* self, const serde_key_t* key, bool value);
    /** @brief Закодировать байты строки. @param self Состояние реализации. @param key Контекстный ключ. @param value Байты. @param length Число байтов. @return Признак успеха. */
    bool (*put_string)(void* self, const serde_key_t* key, const char* value,
                       size_t length);
    /** @brief Закодировать null. @param self Состояние реализации. @param key Контекстный ключ. @return Признак успеха. */
    bool (*put_null)(void* self, const serde_key_t* key);
    /** @brief Завершить документ. @param self Состояние реализации. @param encoded_size Принимает число байтов. @return Признак успеха. */
    bool (*finish)(void* self, size_t* encoded_size);
    /** @brief Проверить состояние средства записи. @param self Состояние реализации. @return Остаётся ли оно допустимым. */
    bool (*ok)(const void* self);
} serde_writer_mtab_t;

/** Копируемый дескриптор экземпляра потокового средства записи. */
typedef struct {
    const serde_writer_mtab_t* mtab; /**< Таблица методов средства записи. */
    void* self;                      /**< Внутреннее состояние в памяти вызывающей стороны. */
} serde_writer_t;

/** Таблица методов, реализуемая кодеком сериализации. */
typedef struct serde_codec_mtab {
    /** @brief Декодировать один блок данных. @param self Состояние кодека. @param data Входные байты. @param size Число байтов. @param root Принимает корневой узел. @return Состояние декодирования. */
    serde_status_t (*decode)(void* self, const uint8_t* data, size_t size,
                             serde_node_t* root);
    /** @brief Определить вид узла. @param self Состояние кодека. @param node Дескриптор узла. @return Семантический вид. */
    serde_kind_t (*kind)(const void* self, serde_node_t node);
    /** @brief Подсчитать элементы объекта. @param self Состояние кодека. @param object Узел объекта. @return Число элементов. */
    size_t (*object_size)(const void* self, serde_node_t object);
    /** @brief Прочитать элемент объекта. @param self Состояние кодека. @param object Узел объекта. @param index Индекс элемента. @param key Принимает представление ключа. @param value Принимает узел значения. @return Признак успеха. */
    bool (*object_member)(const void* self, serde_node_t object, size_t index,
                          serde_key_view_t* key, serde_node_t* value);
    /** @brief Подсчитать элементы массива. @param self Состояние кодека. @param array Узел массива. @return Число элементов. */
    size_t (*array_size)(const void* self, serde_node_t array);
    /** @brief Прочитать элемент массива. @param self Состояние кодека. @param array Узел массива. @param index Индекс элемента. @param value Принимает узел. @return Признак успеха. */
    bool (*array_get)(const void* self, serde_node_t array, size_t index,
                      serde_node_t* value);
    /** @brief Декодировать int32. @param self Состояние кодека. @param node Узел значения. @param value Принимает значение. @return Признак успеха. */
    bool (*get_i32)(const void* self, serde_node_t node, int32_t* value);
    /** @brief Декодировать uint32. @param self Состояние кодека. @param node Узел значения. @param value Принимает значение. @return Признак успеха. */
    bool (*get_u32)(const void* self, serde_node_t node, uint32_t* value);
    /** @brief Декодировать float. @param self Состояние кодека. @param node Узел значения. @param value Принимает значение. @return Признак успеха. */
    bool (*get_f32)(const void* self, serde_node_t node, float* value);
    /** @brief Декодировать логическое значение. @param self Состояние кодека. @param node Узел значения. @param value Принимает значение. @return Признак успеха. */
    bool (*get_bool)(const void* self, serde_node_t node, bool* value);
    /** @brief Декодировать строку. @param self Состояние кодека. @param node Узел строки. @param output Место назначения. @param capacity Размер места назначения в байтах. @param length Принимает длину результата. @return Признак успеха. */
    bool (*get_string)(const void* self, serde_node_t node, char* output,
                       size_t capacity, size_t* length);
    /** @brief Открыть средство записи. @param self Состояние кодека. @param storage Память средства записи. @param output Место назначения. @param capacity Размер места назначения в байтах. @param writer Принимает дескриптор. @return Признак успеха. */
    bool (*writer_open)(void* self, serde_writer_storage_t* storage,
                        uint8_t* output, size_t capacity,
                        serde_writer_t* writer);
} serde_codec_mtab_t;

/** Копируемый дескриптор кодека; состояние `self` должно существовать дольше него. */
typedef struct {
    const serde_codec_mtab_t* mtab; /**< Таблица методов кодека. */
    void* self;                     /**< Состояние реализации кодека. */
} serde_codec_t;

/**
 * @brief Декодировать один полный блок данных.
 * @param codec Инициализированный дескриптор кодека.
 * @param data Входные байты, сохраняемые согласно контракту времени жизни кодека.
 * @param size Число входных байтов.
 * @param root Принимает корневой узел при успешном выполнении.
 * @return Состояние декодирования.
 */
serde_status_t serde_decode(serde_codec_t codec, const uint8_t* data,
                            size_t size, serde_node_t* root);

/**
 * @brief Вернуть семантический вид декодированного узла.
 * @param codec Кодек, создавший @p node.
 * @param node Заимствованный дескриптор декодированного узла.
 * @return Вид узла либо SERDE_KIND_INVALID для недействительного дескриптора.
 */
serde_kind_t serde_kind(serde_codec_t codec, serde_node_t node);

/**
 * @brief Вернуть число элементов объекта.
 * @param codec Кодек, создавший @p object.
 * @param object Заимствованный узел объекта.
 * @return Число элементов либо ноль, если узел не является допустимым объектом.
 */
size_t serde_object_size(serde_codec_t codec, serde_node_t object);

/**
 * @brief Прочитать элемент объекта по позиции с нулевой нумерацией.
 * @param codec Исходный кодек.
 * @param object Узел объекта.
 * @param index Индекс элемента.
 * @param key Принимает заимствованное представление ключа.
 * @param value Принимает узел значения элемента.
 * @return `true`, если @p object и @p index определяют элемент.
 *
 * Возвращённые представление ключа и узел заимствуют текущую память
 * декодирования кодека и становятся недействительными согласно контракту
 * времени жизни декодированных данных этого кодека.
 */
bool serde_object_member(serde_codec_t codec, serde_node_t object,
                         size_t index, serde_key_view_t* key,
                         serde_node_t* value);

/**
 * @brief Найти элемент объекта и подсчитать повторяющиеся совпадения.
 * @param codec Исходный кодек.
 * @param object Узел объекта.
 * @param key Искомый текстовый ключ или тег.
 * @param value Принимает первое совпавшее значение.
 * @param matches Принимает число совпавших ключей; может быть NULL.
 * @return `true`, если найдено хотя бы одно совпадение.
 *
 * Подсчёт повторений позволяет уровню схемы отклонять неоднозначные известные
 * поля, даже если нижележащий формат передачи допускает повторяющиеся ключи.
 */
bool serde_object_get(serde_codec_t codec, serde_node_t object,
                      const serde_key_t* key, serde_node_t* value,
                      size_t* matches);

/**
 * @brief Вернуть число элементов массива.
 * @param codec Кодек, создавший @p array.
 * @param array Заимствованный узел массива.
 * @return Число элементов либо ноль, если узел не является допустимым массивом.
 */
size_t serde_array_size(serde_codec_t codec, serde_node_t array);

/**
 * @brief Прочитать элемент массива по индексу с нулевой нумерацией.
 * @param codec Кодек, создавший @p array.
 * @param array Заимствованный узел массива.
 * @param index Индекс элемента с нулевой нумерацией.
 * @param value Принимает заимствованный узел элемента.
 * @return `true`, если массив и индекс допустимы.
 */
bool serde_array_get(serde_codec_t codec, serde_node_t array, size_t index,
                     serde_node_t* value);

/**
 * @brief Прочитать знаковое 32-битное целое число без преобразования вида.
 * @param codec Кодек, создавший @p node.
 * @param node Читаемый числовой узел.
 * @param value Принимает разобранное целое число.
 * @return `true`, если точное значение представимо типом int32_t.
 */
bool serde_get_i32(serde_codec_t codec, serde_node_t node, int32_t* value);
/**
 * @brief Прочитать беззнаковое 32-битное целое число без преобразования вида.
 * @param codec Кодек, создавший @p node.
 * @param node Читаемый числовой узел.
 * @param value Принимает разобранное целое число.
 * @return `true`, если точное значение представимо типом uint32_t.
 */
bool serde_get_u32(serde_codec_t codec, serde_node_t node, uint32_t* value);
/**
 * @brief Прочитать конечное число одинарной точности.
 * @param codec Кодек, создавший @p node.
 * @param node Читаемый числовой узел.
 * @param value Принимает разобранное значение.
 * @return `true`, если узел числовой, конечный и представим типом float.
 */
bool serde_get_f32(serde_codec_t codec, serde_node_t node, float* value);
/**
 * @brief Прочитать логическое значение без преобразования вида.
 * @param codec Кодек, создавший @p node.
 * @param node Читаемый узел логического значения.
 * @param value Принимает логическое значение.
 * @return `true`, если @p node является допустимым логическим значением.
 */
bool serde_get_bool(serde_codec_t codec, serde_node_t node, bool* value);

/**
 * @brief Декодировать строку в буфер вызывающей стороны с нулевым терминатором.
 * @param codec Исходный кодек.
 * @param node Узел строки.
 * @param output Буфер назначения.
 * @param capacity Размер места назначения с учётом терминатора.
 * @param length Принимает число декодированных байтов без терминатора; может быть NULL.
 * @return `true`, если узел является допустимой строкой и полностью помещается.
 */
bool serde_get_string(serde_codec_t codec, serde_node_t node, char* output,
                      size_t capacity, size_t* length);

/**
 * @brief Открыть потоковое средство записи с ограниченным буфером для формата кодека.
 * @param codec Инициализированный кодек.
 * @param storage Внутренняя память средства записи, принадлежащая вызывающей стороне.
 * @param output Буфер назначения.
 * @param capacity Ёмкость места назначения в байтах.
 * @param writer Принимает дескриптор средства записи.
 * @return `true`, если кодек принял память и выходной буфер.
 * @warning `storage` и `output` должны оставаться действительными до завершения
 *          serde_writer_finish().
 */
bool serde_writer_open(serde_codec_t codec,
                       serde_writer_storage_t* storage, uint8_t* output,
                       size_t capacity, serde_writer_t* writer);

/**
 * @brief Начать значение объекта.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @return `true`, если объект открыт и состояние средства записи остаётся допустимым.
 */
bool serde_begin_object(serde_writer_t writer, const serde_key_t* key);
/**
 * @brief Завершить текущее значение объекта.
 * @param writer Открытое потоковое средство записи, текущий контейнер которого — объект.
 * @return `true`, если объект успешно закрыт.
 */
bool serde_end_object(serde_writer_t writer);
/**
 * @brief Начать значение массива.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @return `true`, если массив открыт и состояние средства записи остаётся допустимым.
 */
bool serde_begin_array(serde_writer_t writer, const serde_key_t* key);
/**
 * @brief Завершить текущее значение массива.
 * @param writer Открытое потоковое средство записи, текущий контейнер которого — массив.
 * @return `true`, если массив успешно закрыт.
 */
bool serde_end_array(serde_writer_t writer);
/**
 * @brief Записать знаковое 32-битное целое число.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @param value Кодируемое целое значение.
 * @return `true` при успехе; `false` устанавливает или сохраняет состояние ошибки.
 */
bool serde_put_i32(serde_writer_t writer, const serde_key_t* key, int32_t value);
/**
 * @brief Записать беззнаковое 32-битное целое число.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @param value Кодируемое целое значение.
 * @return `true` при успехе; `false`, если состояние или ёмкость недопустимы.
 */
bool serde_put_u32(serde_writer_t writer, const serde_key_t* key, uint32_t value);
/**
 * @brief Записать конечное значение одинарной точности.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @param value Кодируемое конечное значение.
 * @return `true` при успехе; `false` для неконечных значений или при ошибке средства записи.
 */
bool serde_put_f32(serde_writer_t writer, const serde_key_t* key, float value);
/**
 * @brief Записать логическое значение.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @param value Кодируемое логическое значение.
 * @return `true` при успехе; `false`, если состояние или ёмкость недопустимы.
 */
bool serde_put_bool(serde_writer_t writer, const serde_key_t* key, bool value);
/**
 * @brief Записать ровно @p length байтов строки с заданным форматом экранирования.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @param value Байты строки; нулевой терминатор не обязателен.
 * @param length Число байтов, читаемых из @p value.
 * @return `true`, если полностью экранированная строка помещается и закодирована.
 */
bool serde_put_string_n(serde_writer_t writer, const serde_key_t* key,
                        const char* value, size_t length);
/**
 * @brief Записать строку с нулевым терминатором и заданным форматом экранирования.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @param value Кодируемая строка с нулевым терминатором.
 * @return `true`, если полностью экранированная строка помещается и закодирована.
 */
bool serde_put_string(serde_writer_t writer, const serde_key_t* key,
                      const char* value);
/**
 * @brief Записать значение null.
 * @param writer Открытое потоковое средство записи.
 * @param key Ключ элемента объекта; NULL в корне или внутри массива.
 * @return `true` при успехе; `false`, если состояние или ёмкость недопустимы.
 */
bool serde_put_null(serde_writer_t writer, const serde_key_t* key);

/**
 * @brief Завершить полный документ средства записи.
 * @param writer Открытое средство записи.
 * @param encoded_size Принимает число закодированных байтов без вспомогательного NUL.
 * @return `true`, только если все контейнеры закрыты и ранее не было ошибок записи.
 */
bool serde_writer_finish(serde_writer_t writer, size_t* encoded_size);

/**
 * @brief Проверить, остаётся ли средство записи допустимым и не переполненным.
 * @param writer Проверяемый дескриптор средства записи.
 * @return `true`, пока не возникло ошибки структуры, значения или ёмкости.
 */
bool serde_writer_ok(serde_writer_t writer);

#ifdef __cplusplus
}
#endif

/** @} */
