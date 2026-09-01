#pragma once

/**
 * @file serde_json.h
 * @brief JSON-реализация не зависящего от формата интерфейса serde.
 *
 * Кодек использует jsmn и не выполняет динамического выделения памяти. Токены
 * jsmn содержат смещения во входном буфере, поэтому при работе с
 * декодированными узлами serde этот буфер не должен изменяться.
 */

#include "serde.h"

#define JSMN_PARENT_LINKS
#ifndef SERDE_JSON_IMPLEMENTATION
#define JSMN_HEADER
#define SERDE_JSON_UNDEF_JSMN_HEADER
#endif
#include "jsmn.h"
#ifdef SERDE_JSON_UNDEF_JSMN_HEADER
#undef JSMN_HEADER
#undef SERDE_JSON_UNDEF_JSMN_HEADER
#endif
#undef JSMN_PARENT_LINKS

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup serde_json Кодек JSON
 * @ingroup serde
 * @brief Адаптер JSON для serde без динамического выделения памяти.
 * @{ */

/** Рекомендуемое число токенов для обычного запроса Krul. */
#define SERDE_JSON_DEFAULT_TOKEN_COUNT 512U

/** Токен JSON, предоставляемый приложением. */
typedef jsmntok_t serde_json_token_t;

/**
 * @brief Изменяемое состояние одного экземпляра кодека JSON.
 *
 * Эта структура и её массив токенов принадлежат приложению. При успешном
 * декодировании заимствованная ссылка на входной кадр сохраняется в `input`;
 * кадр и токены действительны до следующего декодирования этим экземпляром.
 */
typedef struct {
    const uint8_t* input;          /**< Последний успешно декодированный кадр. */
    size_t input_size;             /**< Размер `input` в байтах. */
    serde_json_token_t* tokens;    /**< Массив токенов вызывающей стороны. */
    size_t token_capacity;         /**< Число элементов в `tokens`. */
    int token_count;               /**< Число токенов последнего декодирования. */
} serde_json_codec_t;

/**
 * @brief Инициализировать экземпляр кодека JSON.
 * @param json Инициализируемое состояние кодека.
 * @param tokens Память для токенизации входящих кадров.
 * @param token_capacity Число элементов в @p tokens; должно быть ненулевым.
 * @return `true`, если все аргументы допустимы.
 *
 * Функция не декодирует данные и не получает права владения массивом токенов.
 * Состояние и массив токенов должны существовать дольше всех дескрипторов serde,
 * созданных функцией serde_json().
 */
bool serde_json_init(serde_json_codec_t* json, serde_json_token_t* tokens,
                     size_t token_capacity);

/**
 * @brief Представить кодек JSON через общий интерфейс serde.
 * @param json Инициализированное состояние кодека JSON.
 * @return Легковесный дескриптор serde, заимствующий @p json.
 *
 * Копирование возвращённого дескриптора не копирует состояние кодека и не
 * делает декодированные узлы независимыми от текущего входного кадра.
 */
serde_codec_t serde_json(serde_json_codec_t* json);

/** @} */

#ifdef __cplusplus
}
#endif
