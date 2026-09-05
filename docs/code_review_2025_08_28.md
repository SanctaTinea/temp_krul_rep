# Код-ревью проекта Krul Draft

**Дата:** 2025-08-28  
**Объект:** репозиторий `krul-draft` — библиотеки `krul` / `serde`, Python GUI и симулятор протокола JSON v3.

---

## 1. Краткое резюме

Проект решает задачу **управления встроенным контроллером через декларативный протокол команд** без динамической памяти на стороне прошивки. Архитектура продумана: схема команд — единый источник истины для валидации, discovery и UI; транспорт и формат сериализации отделены от бизнес-логики.

**Общая оценка:** зрелый embedded-friendly дизайн с хорошей документацией протокола и покрытием Python-тестами. Основные риски — **монолитный GUI**, **неполнота draft-репозитория** (нет корневого README и CMake из основного дерева) и **технический долг** в публичном API (`KRUL_TYPE_CONSOLE_STRING`, неформальные комментарии в заголовках).

---

## 2. Состав проекта

| Компонент | Назначение | Объём (оценка) |
| --- | --- | --- |
| `command-interface/` | Диспетчер команд Krul: схема, runtime, dispatch, deferred, discovery | ~2.5k строк C |
| `serialization/` | Абстракция serde + JSON-кодек на jsmn | ~1k строк C |
| `Python GUI/` | PySide6-панель Starset, графики GrathPlot, симулятор | ~4k+ строк Python |
| `docs/` | Спецификация v3, lifecycle, getting started, Doxygen | 4 markdown-файла |
| `command-interface/tests/` | Host-тест `krul_host_test.c` | ~340 строк |

---

## 3. Сильные стороны

### 3.1. Архитектура C-библиотек

Разделение по файлам соответствует фазам обработки запроса:

- `krul_schema.c` — статическая проверка дескрипторов при `krul_server_init()`;
- `krul_runtime.c` — валидация входа и типизированное чтение аргументов;
- `krul_dispatch.c` — конверт запроса/ответа и жизненный цикл deferred;
- `krul_result.c` — проверяемая потоковая запись результата;
- `krul_discovery.c` — сериализация метаданных для GUI;
- `krul.c` — автономные кодировщики error/event/log.

Паттерн **vtable + caller-owned memory** (`serde_codec_t`, `serde_writer_storage_t`) хорошо подходит для MCU: нет `malloc`, предсказуемый расход RAM.

### 3.2. Контракт протокола

Документ `json_protocol_v3.md` нормативен и согласован с реализацией:

- разделение кадров LF;
- ненулевой `id`;
- forward compatibility входа (неизвестные поля игнорируются);
- строгая проверка результата (код ошибки 9);
- единый wire-формат для sync/deferred команд.

Жизненный цикл deferred (handle + generation, очередь completion, один shared response buffer) описан в README command-interface и реализован последовательно в `krul_dispatch.c`.

### 3.3. Безопасность данных на границах

- Дубли известных полей отклоняются (`unique_object_get`, битовая маска `seen`).
- Строки и сообщения об ошибках ограничены (`KRUL_MAX_ERROR_MESSAGE`, `KRUL_MAX_PENDING_ERROR_MESSAGE`).
- JSON writer помечает `failed` и не отдаёт частичный документ как успешный.
- jsmn отвергает непустой хвост после корневого value.
- `krul_encode_log_eventf` отказывает при обрезке, а не молча усекает.

### 3.4. Python-экосистема

- **Симулятор** (`krul_simulator.py`) повторяет discovery, GPIO, ADC, deferred — удобен для разработки без платы.
- **Descriptor-driven UI**: формы строятся из `DESCRIBE`, реестры виджетов (`PARAM_WIDGETS`, `RESULT_WIDGETS`, `COMMAND_WIDGETS`) расширяемы без правок `MainWindow`.
- **GrathPlot** — самодостаточный QPainter-график без тяжёлых зависимостей.
- Набор pytest-тестов покрывает симулятор, registry виджетов, UI (headless через fake transport), ADC/GPIO, графики.

### 3.5. Документация API

Заголовки `krul.h` и `serde.h` содержат Doxygen-комментарии на русском языке; есть `Doxyfile` и ссылки между `@ref`. Для embedded-библиотеки это редкий и ценный уровень детализации.

---

## 4. Замечания по компонентам

### 4.1. Krul — диспетчер (`command-interface/`)

#### Положительное

```286:378:command-interface/src/krul_dispatch.c
krul_dispatch_status_t krul_dispatch(krul_server_t* server,
                                     const uint8_t* request,
                                     size_t request_length) {
    // ...
    if (server->response_ready) return KRUL_DISPATCH_BUSY;
    if (server->completion_count > 0U) {
        (void)krul_pending_encode_next(server);
        return KRUL_DISPATCH_BUSY;
    }
    // decode → validate → handler → finish_response
}
```

Чёткая семантика `BUSY`: транспорт обязан освободить буфер ответа перед новым запросом; при наличии queued completion сначала публикуется отложенный ответ.

Механизм generation в `krul_pending_handle_t` защищает от use-after-free при переиспользовании слотов.

#### Замечания

| Приоритет | Проблема | Детали |
| --- | --- | --- |
| Средний | Линейный поиск команд и полей | `krul_find_command`, `krul_find_field` — O(n). Для десятков команд приемлемо; при сотнях стоит hash/map на этапе init (в flash). |
| Средний | Один response buffer | Архитектурное ограничение; интегратор должен вызывать `krul_response_release` и обрабатывать `KRUL_DISPATCH_BUSY`. Документировано, но легко ошибиться в транспорте. |
| Средний | `KRUL_TYPE_CONSOLE_STRING` | В `krul.h` помечен как «думаю, это выкинуть», но тип участвует в валидации, discovery и GUI. Нужен план миграции или удаление из публичного enum. |
| Низкий | Дублирование `log_severity_name` / `console_name` | Одинаковые switch в `krul.c` и `krul_discovery.c`. |
| Низкий | Неформальные комментарии в публичном API | Фразы вроде «LLM само решило его добавить» в `krul.h` снижают профессиональный тон документации. |
| Низкий | `krul_constraint_fn` / `krul_value_ref_t` | Задокументированы как «пока не планирую использовать», но уже интегрированы в runtime и result. Стоит либо принять как стабильный API, либо пометить `@experimental`. |

#### Предел 64 полей на объект

Используется `uint64_t seen`. В `krul_result_object_complete` для `field_count == 64` явно задан `UINT64_MAX` — корректная обработка краевого случая. Схема не допускает >64 полей на init. **Риск UB отсутствует** при соблюдении контракта.

### 4.2. Serde (`serialization/`)

#### Положительное

- Чистое разделение decode (borrowed nodes) и encode (streaming writer).
- `serde_object_get` считает duplicate keys — важно для JSON, где повтор ключей формально не запрещён парсером.
- `_Static_assert` на размер `json_writer_impl_t` ловит несовместимость с `SERDE_WRITER_STORAGE_SIZE`.

#### Замечания

| Приоритет | Проблема | Детали |
| --- | --- | --- |
| Средний | Лимит токенов jsmn | При `SERDE_ERROR_NO_SPACE` клиент получает «too many elements». Размер массива токенов — ответственность приложения; нужен калькулятор/рекомендация в docs. |
| Низкий | Только JSON в комплекте | README описывает CBOR как будущий кодек — интерфейс готов, реализации нет (ожидаемо для draft). |
| Низкий | `JSON_MAX_DEPTH` (12) vs `KRUL_MAX_RESULT_DEPTH` (8) | Разные пределы; не баг, но стоит явно связать в документации. |

### 4.3. Python GUI (`Python GUI/`)

#### Положительное

- Транспорт вынесен в `SerialWorker` (QThread + queue) — UI не блокируется на I/O.
- Таймауты запросов с периодической проверкой (`_expire_requests`, 250 ms).
- Версия протокола проверяется на этапе `WHOAMI`.
- Темы оформления централизованы в `THEMES`.

#### Замечания

| Приоритет | Проблема | Детали |
| --- | --- | --- |
| **Высокий** | Монолит `Starset.py` (~2900 строк) | `MainWindow`, формы, GPIO, discovery, стили, transport — в одном файле. Затрудняет review, merge и тестирование. Рекомендация: разбить на пакет (`transport.py`, `widgets/`, `discovery.py`, `theme.py`). |
| **Высокий** | README ссылается на `main.py` | Файла `main.py` в репозитории нет; точка входа — `Starset.py`. Инструкция запуска в `Python GUI/README.md` некорректна. |
| Средний | Клиентский код ошибки `-1` при тайм-ауте | `_expire_requests` использует `code: -1`, не входящий в протокол v3 (1–9). Допустимо для локальной ошибки GUI, но может сбить отладку, если клиент ожидает только протокольные коды. |
| Средний | Ответ с неизвестным `id` | `pending.pop(transaction, None)` — ответ без pending молча теряет callback (кроме логирования event/log). При позднем ответе после тайм-аута возможна «orphan» доставка только через terminal. |
| Средний | `errors="replace"` при decode serial | Скрывает битые UTF-8 байты; для debug-режима лучше `strict` или hex-dump. |
| Низкий | Имя `GrathPlot` | Опечатка сохранена намеренно (docstring); для новых разработчиков — источник путаницы. |
| Низкий | Отсутствие type hints в части callback | `Callable` без параметров в `send_request` — слабее, чем остальной код с `from __future__ import annotations`. |

### 4.4. Симулятор (`krul_simulator.py`)

Хорошо структурирован: дескрипторы, stateful GPIO/ADC, deferred, TCP/stdio. Дублирует часть логики discovery из C (`krul_discovery.c`) на Python — **ожидаемо** для standalone-симулятора, но при расширении протокола потребуется синхронизация двух реализаций.

### 4.5. Тесты

| Компонент | Состояние | Замечание |
| --- | --- | --- |
| Python (`tests/`) | Обширный набор | conftest, fake transport, UI через pytest-qt — хорошая практика |
| C (`krul_host_test.c`) | Есть host-тест | Собственный макрос `assert`, не Unity/CMock; нет fuzz-тестов JSON |
| CI | **Не обнаружен** | В draft нет `.github/workflows`, GitLab CI и т.п. |
| Сборка C в draft | **Неполная** | `.gitignore` игнорирует `CMakeLists.txt`; README ссылается на preset `PC_Debug`, которого нет в этом snapshot |

### 4.6. Документация и репозиторий

| Проблема | Описание |
| --- | --- |
| Нет корневого `README.md` | Первый контакт с проектом затруднён; есть только `docs/mainpage.md` и README подкаталогов |
| Пути `Libs/docs/...` в README модулей | В draft дерево называется `docs/`, не `Libs/docs/` — устаревшие пути |
| Нет `LICENSE` | Неясный правовой статус использования |

---

## 5. Безопасность и надёжность

### Что сделано хорошо

- Нет неограниченных копирований строк в C без проверки capacity.
- Deferred slot нельзя завершить дважды (`krul_pending_is_waiting`).
- Повторный defer из completion callback явно отменяется и помечается ошибкой.
- Handler не может оставить незакрытый container (`validate_result` проверяет `depth` и completeness).

### Области внимания

1. **Отсутствие fuzzing** для JSON parser/writer — классический вектор для embedded JSON stacks.
2. **Нет явной защиты от replay** на уровне протокола (только рекомендация не переиспользовать id) — нормально для UART, но стоит документировать для TCP.
3. **Python GUI** не ограничивает размер входящей строки до 10 KiB явно — теоретически возможен большой `readline()`; стоит добавить `MAX_FRAME_SIZE` как в симуляторе.

---

## 6. Согласованность Python ↔ C

| Аспект | Согласованность |
| --- | --- |
| Wire format v3 | Высокая — симулятор и тесты следуют `json_protocol_v3.md` |
| DESCRIBE metadata | Высокая — GUI парсит поля, которые сериализует `krul_discovery.c` |
| Builtin commands | Симулятор дублирует WHOAMI/CMD_LIST/DESCRIBE на Python |
| Error codes | GUI добавляет локальный `-1` для timeout |
| Deferred | Симулятор и C используют одинаковую модель id/success/result |

---

## 7. Рекомендации (приоритизированные)

### Критично / в ближайшем спринте

1. **Добавить корневой `README.md`** с картой репозитория, быстрым стартом и ссылками на `docs/`.
2. **Исправить `Python GUI/README.md`**: заменить `main.py` на `Starset.py` (или добавить тонкий `main.py`-launcher).
3. **Привести пути в README модулей** (`Libs/docs` → `docs` или актуальная структура monorepo).

### Важно / средний срок

4. **Декомпозировать `Starset.py`** на модули; оставить в корне только `Starset.py` как thin entry point.
5. **Решить судьбу `KRUL_TYPE_CONSOLE_STRING`**: удалить, заменить на `STRING` + widget hint, обновить протокол и GUI.
6. **Добавить CI**: pytest на Python; сборка и запуск `krul_host_test` на PC preset.
7. **Лимит размера кадра в GUI** — align с 10 KiB из протокола.

### Желательно / долгий срок

8. Вынести severity/console mapping в одну функцию в C.
9. Заменить custom assert в C-тестах на минимальный test harness с отчётом о прогоне.
10. Добавить fuzz target для `serde_json` (libFuzzer/AFL).
11. Очистить публичные комментарии в `krul.h` от разговорных пометок.
12. Рассмотреть `py.typed` / пакетную структуру для Python-части.

---

## 8. Метрики качества (качественно)

| Критерий | Оценка | Комментарий |
| --- | --- | --- |
| Архитектура | ★★★★★ | Чёткие слои, embedded-first |
| Читаемость C | ★★★★☆ | Хорошие file-level комментарии; местами смешение RU/EN |
| Читаемость Python | ★★★☆☆ | Качество кода хорошее, но монолит снижает оценку |
| Документация протокола | ★★★★★ | Нормативный `json_protocol_v3.md` |
| Тестирование | ★★★★☆ | Python сильный; C базовый; нет CI в draft |
| Сопровождаемость draft-repo | ★★★☆☆ | Неполная сборка, битые ссылки в README |
| Готовность к production (MCU) | ★★★★☆ | Библиотеки готовы; интеграция транспорта — на приложении |

---

## 9. Итог

**Krul + serde** — качественная пара библиотек для протокола команд на ресурсо-ограниченных системах. Дизайн deferred-ответов, статическая валидация схемы и строгая проверка результата handler'а выглядят production-ready при условии корректной интеграции транспорта.

**Python GUI** функционально богат и хорошо протестирован, но **структурно перегружен** одним файлом; это главный технический долг клиентской части.

**Draft-репозиторий** выглядит как вырезка из большего monorepo: документация и тесты на месте, но **инфраструктура сборки и onboarding** требуют доработки перед публикацией как самостоятельного проекта.

---

## 10. Связанные документы

- [mainpage.md](mainpage.md) — обзор библиотек
- [getting_started.md](getting_started.md) — минимальный пример интеграции
- [json_protocol_v3.md](json_protocol_v3.md) — wire-контракт
- [request_lifecycle.md](request_lifecycle.md) — путь запроса и ответа
- [../command-interface/README.md](../command-interface/README.md) — детали Krul
- [../Python GUI/README.md](../Python%20GUI/README.md) — запуск GUI и симулятора
