/*
 * MIT License
 *
 * Copyright (c) 2010 Serge Zaitsev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef JSMN_STATIC
#define JSMN_API static
#else
#define JSMN_API extern
#endif

/**
 * Идентификатор типа JSON. Основные типы:
 * 	o Объект
 * 	o Массив
 * 	o Строка
 * 	o Прочее примитивное значение: число, логическое значение (true/false) или null
 */
typedef enum {
  JSMN_UNDEFINED = 0,
  JSMN_OBJECT = 1 << 0,
  JSMN_ARRAY = 1 << 1,
  JSMN_STRING = 1 << 2,
  JSMN_PRIMITIVE = 1 << 3
} jsmntype_t;

enum jsmnerr {
  /* Предоставлено недостаточно токенов */
  JSMN_ERROR_NOMEM = -1,
  /* Недопустимый символ внутри строки JSON */
  JSMN_ERROR_INVAL = -2,
  /* Строка не содержит полный пакет JSON, ожидаются дополнительные байты */
  JSMN_ERROR_PART = -3
};

/**
 * Описание токена JSON.
 * type		тип (объект, массив, строка и т. д.)
 * start	начальная позиция в строке данных JSON
 * end		конечная позиция в строке данных JSON
 */
typedef struct jsmntok {
  jsmntype_t type;
  int start;
  int end;
  int size;
#ifdef JSMN_PARENT_LINKS
  int parent;
#endif
} jsmntok_t;

/**
 * Парсер JSON. Содержит массив доступных блоков токенов, а также хранит
 * разбираемую строку и текущую позицию в ней.
 */
typedef struct jsmn_parser {
  unsigned int pos;     /* смещение в строке JSON */
  unsigned int toknext; /* следующий выделяемый токен */
  int toksuper;         /* вышестоящий узел токена, например родительский объект или массив */
} jsmn_parser;

/**
 * Создать парсер JSON поверх массива токенов
 */
JSMN_API void jsmn_init(jsmn_parser *parser);

/**
 * Запустить парсер JSON. Он разбирает строку данных JSON в массив токенов,
 * каждый из которых описывает отдельный объект JSON.
 */
JSMN_API int jsmn_parse(jsmn_parser *parser, const char *js, const size_t len,
                        jsmntok_t *tokens, const unsigned int num_tokens);

#ifndef JSMN_HEADER
/**
 * Выделяет новый неиспользованный токен из пула токенов.
 */
static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                   const size_t num_tokens) {
  jsmntok_t *tok;
  if (parser->toknext >= num_tokens) {
    return NULL;
  }
  tok = &tokens[parser->toknext++];
  tok->start = tok->end = -1;
  tok->size = 0;
#ifdef JSMN_PARENT_LINKS
  tok->parent = -1;
#endif
  return tok;
}

/**
 * Заполняет тип и границы токена.
 */
static void jsmn_fill_token(jsmntok_t *token, const jsmntype_t type,
                            const int start, const int end) {
  token->type = type;
  token->start = start;
  token->end = end;
  token->size = 0;
}

/**
 * Заполняет следующий доступный токен примитивным значением JSON.
 */
static int jsmn_parse_primitive(jsmn_parser *parser, const char *js,
                                const size_t len, jsmntok_t *tokens,
                                const size_t num_tokens) {
  jsmntok_t *token;
  int start;

  start = parser->pos;

  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    switch (js[parser->pos]) {
#ifndef JSMN_STRICT
    /* В строгом режиме после примитива должны следовать ",", "}" или "]" */
    case ':':
#endif
    case '\t':
    case '\r':
    case '\n':
    case ' ':
    case ',':
    case ']':
    case '}':
      goto found;
    default:
                   /* подавляет предупреждение gcc */
      break;
    }
    if (js[parser->pos] < 32 || js[parser->pos] >= 127) {
      parser->pos = start;
      return JSMN_ERROR_INVAL;
    }
  }
#ifdef JSMN_STRICT
  /* В строгом режиме после примитива должны следовать запятая, объект или массив */
  parser->pos = start;
  return JSMN_ERROR_PART;
#endif

found:
  if (tokens == NULL) {
    parser->pos--;
    return 0;
  }
  token = jsmn_alloc_token(parser, tokens, num_tokens);
  if (token == NULL) {
    parser->pos = start;
    return JSMN_ERROR_NOMEM;
  }
  jsmn_fill_token(token, JSMN_PRIMITIVE, start, parser->pos);
#ifdef JSMN_PARENT_LINKS
  token->parent = parser->toksuper;
#endif
  parser->pos--;
  return 0;
}

/**
 * Заполняет следующий токен строкой JSON.
 */
static int jsmn_parse_string(jsmn_parser *parser, const char *js,
                             const size_t len, jsmntok_t *tokens,
                             const size_t num_tokens) {
  jsmntok_t *token;

  int start = parser->pos;
  
  /* Пропускаем открывающую кавычку */
  parser->pos++;
  
  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    char c = js[parser->pos];

    /* Кавычка: конец строки */
    if (c == '\"') {
      if (tokens == NULL) {
        return 0;
      }
      token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        parser->pos = start;
        return JSMN_ERROR_NOMEM;
      }
      jsmn_fill_token(token, JSMN_STRING, start + 1, parser->pos);
#ifdef JSMN_PARENT_LINKS
      token->parent = parser->toksuper;
#endif
      return 0;
    }

    /* Обратная косая черта: ожидается экранированный символ */
    if (c == '\\' && parser->pos + 1 < len) {
      int i;
      parser->pos++;
      switch (js[parser->pos]) {
      /* Допустимые экранированные символы */
      case '\"':
      case '/':
      case '\\':
      case 'b':
      case 'f':
      case 'r':
      case 'n':
      case 't':
        break;
      /* Допускается экранированный символ \uXXXX */
      case 'u':
        parser->pos++;
        for (i = 0; i < 4 && parser->pos < len && js[parser->pos] != '\0';
             i++) {
          /* Символ, не являющийся шестнадцатеричным, означает ошибку */
          if (!((js[parser->pos] >= 48 && js[parser->pos] <= 57) ||   /* 0-9 */
                (js[parser->pos] >= 65 && js[parser->pos] <= 70) ||   /* A-F */
                (js[parser->pos] >= 97 && js[parser->pos] <= 102))) { /* a-f */
            parser->pos = start;
            return JSMN_ERROR_INVAL;
          }
          parser->pos++;
        }
        parser->pos--;
        break;
      /* Неожиданный символ */
      default:
        parser->pos = start;
        return JSMN_ERROR_INVAL;
      }
    }
  }
  parser->pos = start;
  return JSMN_ERROR_PART;
}

/**
 * Разобрать строку JSON и заполнить токены.
 */
JSMN_API int jsmn_parse(jsmn_parser *parser, const char *js, const size_t len,
                        jsmntok_t *tokens, const unsigned int num_tokens) {
  int r;
  int i;
  jsmntok_t *token;
  int count = parser->toknext;

  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    char c;
    jsmntype_t type;

    c = js[parser->pos];
    switch (c) {
    case '{':
    case '[':
      count++;
      if (tokens == NULL) {
        break;
      }
      token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        return JSMN_ERROR_NOMEM;
      }
      if (parser->toksuper != -1) {
        jsmntok_t *t = &tokens[parser->toksuper];
#ifdef JSMN_STRICT
        /* В строгом режиме объект или массив не может быть ключом */
        if (t->type == JSMN_OBJECT) {
          return JSMN_ERROR_INVAL;
        }
#endif
        t->size++;
#ifdef JSMN_PARENT_LINKS
        token->parent = parser->toksuper;
#endif
      }
      token->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
      token->start = parser->pos;
      parser->toksuper = parser->toknext - 1;
      break;
    case '}':
    case ']':
      if (tokens == NULL) {
        break;
      }
      type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
#ifdef JSMN_PARENT_LINKS
      if (parser->toknext < 1) {
        return JSMN_ERROR_INVAL;
      }
      token = &tokens[parser->toknext - 1];
      for (;;) {
        if (token->start != -1 && token->end == -1) {
          if (token->type != type) {
            return JSMN_ERROR_INVAL;
          }
          token->end = parser->pos + 1;
          parser->toksuper = token->parent;
          break;
        }
        if (token->parent == -1) {
          if (token->type != type || parser->toksuper == -1) {
            return JSMN_ERROR_INVAL;
          }
          break;
        }
        token = &tokens[token->parent];
      }
#else
      for (i = parser->toknext - 1; i >= 0; i--) {
        token = &tokens[i];
        if (token->start != -1 && token->end == -1) {
          if (token->type != type) {
            return JSMN_ERROR_INVAL;
          }
          parser->toksuper = -1;
          token->end = parser->pos + 1;
          break;
        }
      }
      /* Ошибка при непарной закрывающей скобке */
      if (i == -1) {
        return JSMN_ERROR_INVAL;
      }
      for (; i >= 0; i--) {
        token = &tokens[i];
        if (token->start != -1 && token->end == -1) {
          parser->toksuper = i;
          break;
        }
      }
#endif
      break;
    case '\"':
      r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
      if (r < 0) {
        return r;
      }
      count++;
      if (parser->toksuper != -1 && tokens != NULL) {
        tokens[parser->toksuper].size++;
      }
      break;
    case '\t':
    case '\r':
    case '\n':
    case ' ':
      break;
    case ':':
      parser->toksuper = parser->toknext - 1;
      break;
    case ',':
      if (tokens != NULL && parser->toksuper != -1 &&
          tokens[parser->toksuper].type != JSMN_ARRAY &&
          tokens[parser->toksuper].type != JSMN_OBJECT) {
#ifdef JSMN_PARENT_LINKS
        parser->toksuper = tokens[parser->toksuper].parent;
#else
        for (i = parser->toknext - 1; i >= 0; i--) {
          if (tokens[i].type == JSMN_ARRAY || tokens[i].type == JSMN_OBJECT) {
            if (tokens[i].start != -1 && tokens[i].end == -1) {
              parser->toksuper = i;
              break;
            }
          }
        }
#endif
      }
      break;
#ifdef JSMN_STRICT
    /* В строгом режиме примитивами являются числа и логические значения */
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case 't':
    case 'f':
    case 'n':
      /* Они не должны быть ключами объекта */
      if (tokens != NULL && parser->toksuper != -1) {
        const jsmntok_t *t = &tokens[parser->toksuper];
        if (t->type == JSMN_OBJECT ||
            (t->type == JSMN_STRING && t->size != 0)) {
          return JSMN_ERROR_INVAL;
        }
      }
#else
    /* В нестрогом режиме любое значение без кавычек является примитивом */
    default:
#endif
      r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
      if (r < 0) {
        return r;
      }
      count++;
      if (parser->toksuper != -1 && tokens != NULL) {
        tokens[parser->toksuper].size++;
      }
      break;

#ifdef JSMN_STRICT
    /* Неожиданный символ в строгом режиме */
    default:
      return JSMN_ERROR_INVAL;
#endif
    }
  }

  if (tokens != NULL) {
    for (i = parser->toknext - 1; i >= 0; i--) {
      /* Незакрытый объект или массив */
      if (tokens[i].start != -1 && tokens[i].end == -1) {
        return JSMN_ERROR_PART;
      }
    }
  }

  return count;
}

/**
 * Создаёт новый парсер для заданного буфера с доступным массивом токенов.
 */
JSMN_API void jsmn_init(jsmn_parser *parser) {
  parser->pos = 0;
  parser->toknext = 0;
  parser->toksuper = -1;
}

#endif /* JSMN_HEADER */

#ifdef __cplusplus
}
#endif

#endif /* JSMN_H */
