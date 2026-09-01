# Начало работы {#getting_started}

В этом примере создаётся JSON-сервер Krul с одной синхронной командой `ADD` и
хранилищем, достаточным для двух будущих отложенных команд.

## 1. Предоставьте хранилище для serde и Krul

```c
#include "krul.h"
#include "serde_json.h"

#define RESPONSE_CAPACITY 1024U
#define PENDING_COUNT 2U

static serde_json_token_t json_tokens[SERDE_JSON_DEFAULT_TOKEN_COUNT];
static serde_json_codec_t json;
static uint8_t response_buffer[RESPONSE_CAPACITY];
static krul_pending_slot_t pending_slots[PENDING_COUNT];
static uint16_t completion_queue[PENDING_COUNT];
static krul_server_t server;
```

Всё хранилище статическое. Ни одна из библиотек не вызывает `malloc()`.

## 2. Опишите и реализуйте команду

```c
static const krul_field_desc_t add_params[] = {
    KRUL_I32_REQUIRED("a", "First value", -1000, 1000),
    KRUL_I32_REQUIRED("b", "Second value", -1000, 1000),
};

static const krul_field_desc_t add_result[] = {
    KRUL_RESULT_I32("sum", "Sum"),
};

static bool add_handler(const krul_args_t *args,
                        krul_result_t *result,
                        krul_error_t *error)
{
    int32_t a;
    int32_t b;
    (void)error;

    if (!krul_args_get_i32(args, "a", &a) ||
        !krul_args_get_i32(args, "b", &b))
        return false;

    return krul_result_put_i32(result, "sum", a + b);
}

static const krul_command_t add_command = {
    .name = "ADD",
    .title = "Add two values",
    .params = add_params,
    .params_count = KRUL_ARRAY_SIZE(add_params),
    .result = add_result,
    .result_count = KRUL_ARRAY_SIZE(add_result),
    .handler = add_handler,
};

static const krul_command_t *const commands[] = {
    &add_command,
};
```

Дескриптор служит единым источником истины для проверок во время выполнения и
метаданных обнаружения Krul.

## 3. Инициализируйте сервер

```c
bool commands_init(void)
{
    if (!serde_json_init(&json, json_tokens,
                         KRUL_ARRAY_SIZE(json_tokens)))
        return false;

    const krul_server_config_t config = {
        .commands = commands,
        .command_count = KRUL_ARRAY_SIZE(commands),
        .device_name = "EXAMPLE",
        .firmware_version = "1.0.0",
        .protocol_version = 4,
        .codec = serde_json(&json),
        .response_buffer = response_buffer,
        .response_capacity = sizeof(response_buffer),
        .pending_slots = pending_slots,
        .pending_slot_count = KRUL_ARRAY_SIZE(pending_slots),
        .completion_queue = completion_queue,
    };

    return krul_server_init(&server, &config);
}
```

Для сервера только с синхронными командами не указывайте `pending_slots`,
`pending_slot_count` и `completion_queue`.

## 4. Передайте на обработку один полный транспортный кадр

```c
static void send_responses(void);

bool handle_frame(const uint8_t *frame, size_t length)
{
    krul_dispatch_status_t status = krul_dispatch(&server, frame, length);

    if (status == KRUL_DISPATCH_BUSY)
        return false; /* Сохраните входной кадр и повторите после отправки ответа. */

    if (status == KRUL_DISPATCH_FAILED)
        return true;  /* Кадр принят, но сформировать ответ не удалось. */

    if (status == KRUL_DISPATCH_RESPONSE_READY)
        send_responses();

    return true;
}
```

`frame` должен содержать одну полную полезную нагрузку без transport v1
заголовка и CRC. Используйте `krul_transport_parser_consume()` для выделения
`KRJ1`/`KRB1`/`KRC1` из потока и `krul_transport_encode()` для обрамления ответа.

## 5. Отправьте и освободите ответы

```c
void send_responses(void)
{
    while (krul_pending_encode_next(&server)) {
        const uint8_t *data = krul_response_get_data(&server);
        size_t size = krul_response_get_size(&server);

        if (!transport_try_copy(data, size))
            return; /* Оставьте ответ опубликованным и повторите позже. */

        krul_response_release(&server);
    }
}
```

Вызов `krul_pending_encode_next()` безопасен для серверов только с синхронными командами.
Для сервера с отложенными командами вызывайте `send_responses()` из основного
цикла после обработки событий завершения аппаратных или удалённых операций.

## 6. Выполните пробный запрос

Входной кадр:

```json
{"cmd":"ADD","id":1,"params":{"a":20,"b":22}}
```

Выходной кадр:

```json
{"id":1,"success":true,"result":{"sum":42}}
```

Шаблон отложенного обработчика и правила владения приведены в разделе
@ref request_lifecycle.

## Необязательно: отложите длительную операцию

Скопируйте все значения, которые понадобятся после возврата из обработчика, в
стабильное хранилище приложения. Pending-слот хранит только указатель на это
хранилище:

```c
typedef struct {
    krul_pending_handle_t handle;
    int32_t input;
    int32_t output;
    bool in_use;
} work_t;

static work_t work;

static bool work_complete(krul_result_t *result, krul_error_t *error, void *context) {
    work_t *item = context;
    (void)error;
    return krul_result_put_i32(result, "value", item->output);
}

static void work_release(void *context) {
    ((work_t *)context)->in_use = false;
}

static bool start_handler(const krul_args_t *args,
                          krul_result_t *result,
                          krul_error_t *error) {
    if (work.in_use) {
        krul_error_set(error, KRUL_ERROR_EXECUTION, "Worker is busy");
        return false;
    }

    if (!krul_args_get_i32(args, "value", &work.input))
        return false;

    work.in_use = true;
    if (!krul_result_defer(result, &work, work_complete, work_release, &work.handle)) {
        work.in_use = false;
        return false;
    }

    hardware_start(work.input);
    return true;
}
```

Когда оборудование завершит работу, сохраните результат в `work.output`. Из
того же сериализованного контекста, которому принадлежит сервер Krul, вызовите:

```c
if (hardware_done) {
    work.output = hardware_result;
    krul_pending_complete(&server, work.handle);
}

send_responses();
```

Если завершение обнаруживает обработчик прерывания, он должен передать флаг или
элемент очереди задаче сервера, а не вызывать Krul параллельно. `work_release()`
выполняется только после того, как транспорт освободит сериализованный ответ
(либо запрос будет отменён), поэтому контекст не будет повторно использован
слишком рано.


Немного объясню про слоты сохранения:
Их количество задаётся дефайном PENDING_COUNT. У каждого слота есть 4 состояния
1. KRUL_PENDING_FREE
2. KRUL_PENDING_ACTIVE
3. KRUL_PENDING_COMPLETION_QUEUED,
4. KRUL_PENDING_ENCODED

1. В начале все слоты находятся в KRUL_PENDING_FREE.
krul_pending_is_active(server, handle)  == false
krul_pending_is_waiting(server, handle) == false
Вызов krul_result_defer() переводит в KRUL_PENDING_ACTIVE

2. KRUL_PENDING_ACTIVE
Сейчас что-то аппаратно выполняется и мы занимаемся своими делами.
krul_pending_is_active(server, handle)  == true
krul_pending_is_waiting(server, handle) == true
Вызов krul_pending_cancel() отменяет команду и идёт в KRUL_PENDING_FREE
Вызов krul_pending_complete() или krul_pending_fail() означает, что аппаратно что-то выполнилось, или провалилось, но тем-неменее мы переходим в KRUL_PENDING_COMPLETION_QUEUED

3. KRUL_PENDING_COMPLETION_QUEUED
Слот поставлен в FIFO завершённых операций и ждёт krul_pending_encode_next()
krul_pending_is_active(server, handle)  == true
krul_pending_is_waiting(server, handle) == false
Вызов krul_pending_encode_next() (ИМННО ОН, И ТОЛЬКО ОН ВЫЗЫВАЕТ ПОЛЬЗОВАТЕЛЬСКИЙ completion) ведёк к KRUL_PENDING_ENCODED

4. KRUL_PENDING_ENCODED
krul_pending_is_active(server, handle)  == true
krul_pending_is_waiting(server, handle) == false
После окончания отправки пакета траснпортный уровень должен вызвать krul_response_release, это всё отчищает и переводит слот в состояние KRUL_PENDING_FREE.
