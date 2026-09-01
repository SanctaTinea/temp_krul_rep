# JSON-протокол Krul версии 3 {#json_protocol_v3}

Этот документ задаёт нормативный wire-контракт JSON-профиля Krul версии 3,
реализованный библиотекой Krul, прошивками ARK и BKU/CVM, host-сервером,
Python GUI и симулятором. C API описан отдельно в @ref getting_started и
@ref request_lifecycle.

## 1. Возможности версии 3

Текущая версия протокола — **3**. Она объединяет в один поддерживаемый контракт:

- UTF-8 JSON внутри transport v1 frame с magic `KRJ1`, длиной и CRC;
- ненулевые 32-битные ID транзакций;
- единые success/error-конверты;
- рекурсивные схемы, defaults и проверку входа и результата;
- `WHOAMI`, `CMD_LIST` и `DESCRIBE`;
- синхронные и deferred-команды с одинаковым wire-форматом;
- события, структурированные логи, UI-метаданные, autoupdate и таймауты;
- скрытые от GUI команды `nogui`.

Переход с версии 2 на 3 меняет handshake. Структура базовых конвертов сохранена,
но клиент не должен молча считать другую major-версию совместимой.

## 2. Транспортный профиль ARK/BKU

Krul сам не выполняет UART/TCP I/O и получает один готовый payload. Приложения
репозитория выделяют его по @ref transport_v1:

- magic JSON-кадра — ASCII `KRJ1`;
- `payload_length` содержит длину одного JSON object в UTF-8;
- payload не содержит транспортного LF или завершающего `NUL`;
- максимальный payload — 10 КиБ (10 240 байт);
- после payload передаётся CRC-16/CCITT-FALSE;
- применяются стандартные JSON escapes, включая кавычки и переводы строк;
- комментарии, trailing comma, `NaN` и бесконечности запрещены.

Пробелы и порядок полей object незначимы.

```text
KRJ1 | 17 00 00 00 | {"cmd":"WHOAMI","id":1} | CRC16
```

## 3. Сообщения и ID

| Вид | Отличительное поле | `id` |
| --- | --- | --- |
| Запрос | `cmd` | обязателен, 1…4 294 967 295 |
| Ответ | `success` | повторяет ID запроса |
| Событие | `event` | отсутствует |

Ответы сопоставляются по ID, а не по порядку прихода. Клиенту рекомендуется не
повторять ID, пока соответствующий запрос не завершён. Ноль зарезервирован для
ошибок, возникших до чтения корректного ID.

## 4. Запрос

```json
{
  "cmd": "COMMAND_NAME",
  "id": 42,
  "params": {"parameter": "value"}
}
```

| Поле | Тип | Правило |
| --- | --- | --- |
| `cmd` | string | Обязательно; точное регистрозависимое имя, до 47 декодированных байт UTF-8. |
| `id` | integer | Обязательно; ненулевой `uint32`. |
| `params` | object | Можно опустить, если параметров нет или каждый параметр верхнего уровня имеет default. |

Неизвестные поля запроса игнорируются для совместимости. Повторные `cmd`, `id`
или `params` запрещены. Клиенту следует избегать любых повторных JSON-имён.

## 5. Ответы

Успех:

```json
{"id":42,"success":true,"result":{"value":123}}
```

`result` всегда object; при отсутствии полей он равен `{}`.

Ошибка:

```json
{
  "id": 42,
  "success": false,
  "error": {"code": 3, "message": "Value is out of range"}
}
```

`code` стабилен и предназначен для программы. Точный текст bounded-поля
`message` не является частью контракта.

| Код | C-константа | Значение |
| ---: | --- | --- |
| 0 | `KRUL_OK` | Успех; в error-конверте не используется. |
| 1 | `KRUL_ERROR_MISSING_FIELD` | Нет обязательного поля. |
| 2 | `KRUL_ERROR_INVALID_TYPE` | Неверный семантический тип. |
| 3 | `KRUL_ERROR_OUT_OF_RANGE` | Нарушен диапазон числа, длины, enum, массива или ID. |
| 4 | `KRUL_ERROR_MALFORMED_PAYLOAD` | Некорректный JSON, корень не object, лишний хвост или не хватило токенов. |
| 5 | `KRUL_ERROR_INVALID_REQUEST` | Неоднозначная структура, например повтор известного поля. |
| 6 | `KRUL_ERROR_UNKNOWN_COMMAND` | Неизвестная команда или цель `DESCRIBE`. |
| 7 | `KRUL_ERROR_EXECUTION` | Ошибка команды, устройства, таймаута или deferred-операции. |
| 8 | `KRUL_ERROR_RESPONSE_TOO_LARGE` | Ответ не помещается в response-буфер. |
| 9 | `KRUL_ERROR_INVALID_RESULT` | Handler сформировал результат не по схеме. |

Busy response-буфер и транспортная ошибка сами не создают JSON-ответ: транспорт
должен сохранить непринятый запрос и повторить его позднее.

## 6. Самоописание

### `WHOAMI`

```json
{"cmd":"WHOAMI","id":1}
```

```json
{
  "id": 1,
  "success": true,
  "result": {
    "protocol_version": 3,
    "device_name": "ARK",
    "device_id": "003D002E3432511233333035",
    "firmware": "1.0.0"
  }
}
```

`device_id` необязателен для совместимости со старыми устройствами. Если поле
присутствует, оно должно стабильно идентифицировать физический экземпляр и не
меняться при обновлении прошивки.

### `CMD_LIST`

```json
{"cmd":"CMD_LIST","id":2}
```

```json
{
  "id": 2,
  "success": true,
  "result": {
    "cmd_name": ["WHOAMI", "CMD_LIST", "DESCRIBE", "ADC_READ"]
  }
}
```

Список содержит все команды, включая built-ins и `nogui`.

### `DESCRIBE`

```json
{"cmd":"DESCRIBE","id":3,"params":{"name":"ADC_READ"}}
```

Успешный `result` содержит дескриптор команды. Необязательные значения
пропускаются, а не кодируются как `null`, `false`, ноль или пустой массив.

## 7. Дескриптор команды

```json
{
  "cmd": "ADC_READ",
  "tab": "Measurements",
  "title": "Read ADC",
  "description": "Read all ADC channels.",
  "group": "Monitoring",
  "widget_hint": "special_adc_group",
  "order": 20,
  "timeout_ms": 1500,
  "result": [
    {"name": "value_VBAT", "label": "VBAT", "type": "integer"}
  ],
  "autoupdate": {
    "min_period": 100,
    "max_period": 1000,
    "default_period": 500
  }
}
```

| Поле | Тип | Назначение |
| --- | --- | --- |
| `cmd` | string | Wire-имя; всегда присутствует. |
| `builtin` | boolean | `true` у discovery-команд; обычная форма не строится. |
| `nogui` | boolean | `true` у вызываемой команды, которую GUI полностью скрывает. Она остаётся в `CMD_LIST`. |
| `tab` | string | Необязательная вкладка. |
| `title` | string | Заголовок; всегда присутствует, по умолчанию равен `cmd`. |
| `description` | string | Необязательное пояснение. |
| `group` | string | Необязательная группа. |
| `widget_hint` | string | Необязательная подсказка представления. |
| `order` | unsigned | Ненулевой порядок сортировки. |
| `timeout_ms` | unsigned | Таймаут команды в мс; клиент может добавить транспортный запас. |
| `params` | array | Дескрипторы параметров; отсутствует для пустого списка. |
| `result` | array | Дескрипторы результата; отсутствует для пустого списка. |
| `autoupdate` | object | Рекомендация автоопроса. |

`autoupdate` содержит unsigned `min_period`, `max_period`, `default_period` в
миллисекундах и не заставляет сервер самостоятельно отправлять ответы.

Стандартные `widget_hint`: `slider`, `spinbox`, `special_adc`,
`special_adc_group`, `special_gpio`. Неизвестный hint разрешено показать
обычным виджетом.

## 8. Дескриптор поля

```json
{
  "name": "samples",
  "tag": 10,
  "label": "Samples",
  "type": "unsigned",
  "widget_hint": "spinbox",
  "constraints": {"minimum": 1, "maximum": 1024},
  "default": 1
}
```

| Поле | Правило |
| --- | --- |
| `name` | Обязательно для поля object; отсутствует у дескриптора элемента array. |
| `tag` | Необязательный ненулевой ключ компактного кодека; JSON использует `name`. |
| `label` | Необязательная подпись. |
| `type` | Обязательный семантический тип. |
| `widget_hint` | Необязательная UI-подсказка. |
| `constraints` | Встроенные ограничения типа. |
| `default` | Default входа; делает поле необязательным. |
| `items` | Единый дескриптор элементов array. |
| `fields` | Дескрипторы полей object. |

| `type` | JSON | `constraints` | Default |
| --- | --- | --- | --- |
| `integer` | целое number | signed `minimum`, `maximum` (`int32`) | number |
| `unsigned` | целое number | unsigned `minimum`, `maximum` (`uint32`) | number |
| `float` | конечное number | `minimum`, `maximum`, `step` | number |
| `string` | string | `minLength`, `maxLength` | string |
| `boolean` | boolean | нет | boolean |
| `enum` | string | `values`: `{value,title}[]` | string |
| `array` | array | `minItems`, `maxItems` | пустой array |
| `object` | object | рекурсивное `fields` | нет |
| `console_string` | string | `severity`, `maxLength` | нет |

`step` у float — шаг UI: сервер проверяет конечность и включительный диапазон,
но не кратность step. В enum по wire передаётся `value`, а `title` служит
подписью. `console_string.severity`: `debug`, `info`, `warning`, `error`.

Пользовательский validator может вводить правила, не выраженные в `DESCRIBE`.
Окончательное решение о допустимости всегда принимает сервер.

## 9. Валидация

До handler сервер проверяет:

- корень и `params` имеют тип object;
- известное поле встречается не более одного раза;
- отсутствующее поле имеет default;
- неизвестные входные поля рекурсивно игнорируются;
- integer не принимает дробь, unsigned — дробь или отрицательное число;
- числа, длины и размеры входят во включительные диапазоны;
- enum, array и object рекурсивно соответствуют схеме;
- custom validator проходит после встроенной проверки.

Defaults читает typed accessor handler; входной JSON не изменяется.

Результат также проверяется: каждое объявленное поле записывается ровно один
раз, неизвестные/повторные поля запрещены, типы и диапазоны соблюдаются,
контейнеры закрываются. Текущие пределы — 64 поля в object схемы и восемь
уровней результата. Ошибка handler превращается в код 9, а не в битый success.

## 10. Deferred-команды

Deferred-запрос не получает промежуточного ACK и позднее завершается ровно одним
обычным ответом с исходным ID. Несколько запросов могут выполняться параллельно,
а ответы идут в порядке очереди завершения, не обязательно запроса.

Response-буфер один. Пока транспорт не принял полный кадр и не освободил его,
следующий ответ не публикуется. Это backpressure реализации, а не новый вид JSON.

## 11. События

Событие не содержит ID:

```json
{
  "event": "temperature_alarm",
  "data": {"channel": "BOARD", "temperature": 91.5, "active": true}
}
```

Текущий общий encoder поддерживает в `data` signed/unsigned integers, конечные
floats, booleans и strings. Схемы прикладных событий задаёт приложение.

Стандартный журнал:

```json
{
  "event": "log",
  "data": {"severity": "warning", "message": "MRAM is not responding"}
}
```

`severity`: `debug`, `info`, `warning`, `error`. Неизвестный уровень следует
показать нейтральным стилем, не отвергая кадр.

## 12. Алгоритм клиента

1. Вызвать `WHOAMI` и потребовать `protocol_version == 3`.
2. Получить `CMD_LIST`.
3. Вызвать `DESCRIBE` для каждого имени.
4. Не строить обычные формы для `builtin`.
5. Сохранить `nogui` для raw/программных вызовов, но не строить для них никакие
   обычные или специализированные GUI-элементы.
6. Построить остальные формы по схемам и UI-метаданным.
7. Назначать новый ненулевой ID и сопоставлять ответы по ID.
8. Обрабатывать события независимо от незавершённых запросов.

Клиент должен терпимо относиться к неизвестным полям object, event и widget
hints. Неизвестный тип нельзя молча трактовать как другой тип.

## 13. Полный пример

```json
{
  "cmd": "SET_LEVEL",
  "title": "Set output level",
  "params": [
    {
      "name": "channel",
      "type": "enum",
      "constraints": {
        "values": [
          {"value": "A", "title": "Channel A"},
          {"value": "B", "title": "Channel B"}
        ]
      }
    },
    {
      "name": "level",
      "type": "unsigned",
      "widget_hint": "slider",
      "constraints": {"minimum": 0, "maximum": 100},
      "default": 50
    }
  ],
  "result": [{"name": "applied", "type": "boolean"}]
}
```

Запрос использует default `level`:

```json
{"cmd":"SET_LEVEL","id":100,"params":{"channel":"A"}}
```

```json
{"id":100,"success":true,"result":{"applied":true}}
```

## 14. Расширение версии 3

Сервер может добавлять команды, необязательные поля и события. Клиент игнорирует
неизвестные поля object; сервер — неизвестные поля запроса и params, включая
вложенные. Известные поля сохраняют значение и JSON-тип.

Изменение конверта, транзакционной семантики, значения кода ошибки или
обязательного discovery-поведения требует новой версии. `protocol_version`
описывает wire-контракт, а выпуск прошивки остаётся отдельной строкой `firmware`.
