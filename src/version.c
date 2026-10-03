#include "version.h"

#include "common.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t start, end;
} Range;
typedef struct {
  const char *text;
  size_t position, length;
} Scanner;

typedef struct {
  Range root_version, root_object, private_value, lock_version,
      lock_package_version;
  Range lock_package_object;
  bool has_private, is_private, has_root_version, has_lock_version,
      has_lock_package;
  bool has_lock_package_version;
} JsonFields;

static int read_json_hex4(const char *text, size_t length, size_t *position,
                          unsigned int *value);

static void set_error(char *error, size_t size, const char *message) {
  if (error != NULL && size > 0)
    snprintf(error, size, "%s", message);
}

static void spaces(Scanner *s) {
  while (s->position < s->length &&
         (s->text[s->position] == ' ' || s->text[s->position] == '\t' ||
          s->text[s->position] == '\r' || s->text[s->position] == '\n'))
    ++s->position;
}

static int append_decoded_json_codepoint(char *out, size_t out_size,
                                         size_t *used, unsigned int value) {
  char bytes[4];
  size_t count;
  if (value >= 0xd800 && value <= 0xdfff)
    value = 0xfffd;
  if (value <= 0x7f) {
    bytes[0] = (char)value;
    count = 1;
  } else if (value <= 0x7ff) {
    bytes[0] = (char)(0xc0 | (value >> 6));
    bytes[1] = (char)(0x80 | (value & 0x3f));
    count = 2;
  } else if (value <= 0xffff) {
    bytes[0] = (char)(0xe0 | (value >> 12));
    bytes[1] = (char)(0x80 | ((value >> 6) & 0x3f));
    bytes[2] = (char)(0x80 | (value & 0x3f));
    count = 3;
  } else if (value <= 0x10ffff) {
    bytes[0] = (char)(0xf0 | (value >> 18));
    bytes[1] = (char)(0x80 | ((value >> 12) & 0x3f));
    bytes[2] = (char)(0x80 | ((value >> 6) & 0x3f));
    bytes[3] = (char)(0x80 | (value & 0x3f));
    count = 4;
  } else
    return 0;
  if (out != NULL) {
    if (*used >= out_size || count >= out_size - *used)
      return 0;
    memcpy(out + *used, bytes, count);
    *used += count;
  }
  return 1;
}

static int string_value(Scanner *s, char *out, size_t out_size,
                        size_t *out_length, Range *range) {
  size_t used = 0;
  if (s->position >= s->length || s->text[s->position] != '"')
    return 0;
  if (range != NULL)
    range->start = s->position;
  ++s->position;
  while (s->position < s->length) {
    unsigned char c = (unsigned char)s->text[s->position++];
    if (c < 0x20)
      return 0;
    if (c == '"') {
      if (out != NULL) {
        if (used >= out_size)
          return 0;
        out[used] = '\0';
      }
      if (out_length != NULL)
        *out_length = used;
      if (range != NULL)
        range->end = s->position;
      return 1;
    }
    if (c == '\\') {
      if (s->position >= s->length)
        return 0;
      c = s->text[s->position++];
      if (c == 'u') {
        unsigned int value, low;
        if (!read_json_hex4(s->text, s->length, &s->position, &value))
          return 0;
        if (value >= 0xd800 && value <= 0xdbff &&
            s->position + 6 <= s->length && s->text[s->position] == '\\' &&
            s->text[s->position + 1] == 'u') {
          size_t low_position = s->position + 2;
          if (read_json_hex4(s->text, s->length, &low_position, &low) &&
              low >= 0xdc00 && low <= 0xdfff) {
            value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
            s->position = low_position;
          }
        }
        if (!append_decoded_json_codepoint(out, out_size, &used, value))
          return 0;
        continue;
      } else if (c == 'b')
        c = '\b';
      else if (c == 'f')
        c = '\f';
      else if (c == 'n')
        c = '\n';
      else if (c == 'r')
        c = '\r';
      else if (c == 't')
        c = '\t';
      else if (c != '"' && c != '\\' && c != '/')
        return 0;
    }
    if (out != NULL) {
      if (used + 1 >= out_size)
        return 0;
      out[used++] = (char)c;
    }
  }
  return 0;
}

static int skip_value(Scanner *s, Range *range);
static int skip_object(Scanner *s) {
  if (s->text[s->position++] != '{')
    return 0;
  spaces(s);
  while (s->position < s->length && s->text[s->position] != '}') {
    if (!string_value(s, NULL, 0, NULL, NULL))
      return 0;
    spaces(s);
    if (s->position >= s->length || s->text[s->position++] != ':')
      return 0;
    spaces(s);
    if (!skip_value(s, NULL))
      return 0;
    spaces(s);
    if (s->position < s->length && s->text[s->position] == ',') {
      ++s->position;
      spaces(s);
      if (s->position >= s->length || s->text[s->position] == '}')
        return 0;
    } else if (s->position >= s->length || s->text[s->position] != '}')
      return 0;
  }
  if (s->position >= s->length)
    return 0;
  ++s->position;
  return 1;
}
static int skip_array(Scanner *s) {
  if (s->text[s->position++] != '[')
    return 0;
  spaces(s);
  while (s->position < s->length && s->text[s->position] != ']') {
    if (!skip_value(s, NULL))
      return 0;
    spaces(s);
    if (s->position < s->length && s->text[s->position] == ',') {
      ++s->position;
      spaces(s);
      if (s->position >= s->length || s->text[s->position] == ']')
        return 0;
    } else if (s->position >= s->length || s->text[s->position] != ']')
      return 0;
  }
  if (s->position >= s->length)
    return 0;
  ++s->position;
  return 1;
}
static int json_value_terminator(char character) {
  return character == ',' || character == '}' || character == ']' ||
         character == ' ' || character == '\t' || character == '\r' ||
         character == '\n';
}

static int skip_json_number(Scanner *scanner) {
  size_t position = scanner->position;
  if (scanner->text[position] == '-')
    ++position;
  if (position >= scanner->length)
    return 0;
  if (scanner->text[position] == '0') {
    ++position;
    if (position < scanner->length &&
        isdigit((unsigned char)scanner->text[position]))
      return 0;
  } else if (scanner->text[position] >= '1' && scanner->text[position] <= '9') {
    do {
      ++position;
    } while (position < scanner->length &&
             isdigit((unsigned char)scanner->text[position]));
  } else {
    return 0;
  }
  if (position < scanner->length && scanner->text[position] == '.') {
    ++position;
    if (position >= scanner->length ||
        !isdigit((unsigned char)scanner->text[position]))
      return 0;
    do {
      ++position;
    } while (position < scanner->length &&
             isdigit((unsigned char)scanner->text[position]));
  }
  if (position < scanner->length &&
      (scanner->text[position] == 'e' || scanner->text[position] == 'E')) {
    ++position;
    if (position < scanner->length &&
        (scanner->text[position] == '+' || scanner->text[position] == '-'))
      ++position;
    if (position >= scanner->length ||
        !isdigit((unsigned char)scanner->text[position]))
      return 0;
    do {
      ++position;
    } while (position < scanner->length &&
             isdigit((unsigned char)scanner->text[position]));
  }
  if (position < scanner->length &&
      !json_value_terminator(scanner->text[position]))
    return 0;
  scanner->position = position;
  return 1;
}

static int skip_json_literal(Scanner *scanner, const char *literal,
                             size_t literal_size) {
  if (scanner->length - scanner->position < literal_size ||
      memcmp(scanner->text + scanner->position, literal, literal_size) != 0)
    return 0;
  scanner->position += literal_size;
  return scanner->position == scanner->length ||
         json_value_terminator(scanner->text[scanner->position]);
}

static int skip_value(Scanner *s, Range *range) {
  size_t start = s->position;
  char first;
  if (start >= s->length)
    return 0;
  first = s->text[start];
  if (first == '"')
    return string_value(s, NULL, 0, NULL, range);
  if (first == '{' || first == '[') {
    int ok = first == '{' ? skip_object(s) : skip_array(s);
    if (ok && range != NULL) {
      range->start = start;
      range->end = s->position;
    }
    return ok;
  }
  if (first == 't') {
    if (!skip_json_literal(s, "true", 4))
      return 0;
  } else if (first == 'f') {
    if (!skip_json_literal(s, "false", 5))
      return 0;
  } else if (first == 'n') {
    if (!skip_json_literal(s, "null", 4))
      return 0;
  } else if (first == '-' || isdigit((unsigned char)first)) {
    if (!skip_json_number(s))
      return 0;
  } else {
    return 0;
  }
  if (range != NULL) {
    range->start = start;
    range->end = s->position;
  }
  return 1;
}

static int object_field(Scanner *s, const char *wanted, Range *range,
                        char *decoded, size_t decoded_size) {
  int found = 0;
  if (s->position >= s->length || s->text[s->position++] != '{')
    return 0;
  spaces(s);
  while (s->position < s->length && s->text[s->position] != '}') {
    char key[128];
    size_t key_length;
    Range value;
    if (!string_value(s, key, sizeof key, &key_length, NULL))
      return 0;
    spaces(s);
    if (s->position >= s->length || s->text[s->position++] != ':')
      return 0;
    spaces(s);
    if (key_length == strlen(wanted) && memcmp(key, wanted, key_length) == 0) {
      if (decoded != NULL) {
        size_t decoded_length;
        if (!string_value(s, decoded, decoded_size, &decoded_length, &value) ||
            decoded_length != strlen(decoded))
          return 0;
      } else if (!skip_value(s, &value))
        return 0;
      if (range != NULL)
        *range = value;
      found = 1;
    } else if (!skip_value(s, NULL))
      return 0;
    spaces(s);
    if (s->position < s->length && s->text[s->position] == ',') {
      ++s->position;
      spaces(s);
    } else if (s->position >= s->length || s->text[s->position] != '}')
      return 0;
  }
  if (s->position >= s->length)
    return 0;
  ++s->position;
  return found;
}

static int json_object_range(const char *content, Range *range) {
  Scanner scanner = {content, 0, strlen(content)};
  spaces(&scanner);
  if (scanner.position >= scanner.length ||
      scanner.text[scanner.position] != '{')
    return 0;
  range->start = scanner.position;
  if (!skip_object(&scanner))
    return 0;
  range->end = scanner.position;
  spaces(&scanner);
  return scanner.position == scanner.length;
}

static int json_empty_root_object(const char *content, size_t *position) {
  Scanner scanner = {content, 0, strlen(content)};
  spaces(&scanner);
  if (scanner.position >= scanner.length ||
      scanner.text[scanner.position++] != '{')
    return 0;
  spaces(&scanner);
  if (scanner.position != scanner.length)
    return 0;
  *position = scanner.position;
  return 1;
}

static void json_position_to_line_column(const char *content, size_t position,
                                         size_t *line, size_t *column) {
  size_t i;
  *line = 1;
  *column = 1;
  for (i = 0; i < position; ++i) {
    if (content[i] == '\r') {
      ++*line;
      *column = 1;
      if (i + 1 < position && content[i + 1] == '\n')
        ++i;
    } else if (content[i] == '\n') {
      ++*line;
      *column = 1;
    } else {
      ++*column;
    }
  }
}

static int json_trailing_comma_position(const char *content, char closing,
                                        size_t *position) {
  bool in_string = false, escaped = false;
  size_t length = strlen(content), i;
  for (i = 0; i < length; ++i) {
    if (in_string) {
      if (escaped)
        escaped = false;
      else if (content[i] == '\\')
        escaped = true;
      else if (content[i] == '"')
        in_string = false;
      continue;
    }
    if (content[i] == '"') {
      in_string = true;
      continue;
    }
    if (content[i] == ',') {
      size_t next = i + 1;
      while (next < length && (content[next] == ' ' || content[next] == '\t' ||
                               content[next] == '\r' || content[next] == '\n'))
        ++next;
      if (next < length && content[next] == closing) {
        *position = next;
        return 1;
      }
    }
  }
  return 0;
}

static int json_leading_zero_position(const char *content, size_t *position) {
  bool in_string = false, escaped = false;
  size_t length = strlen(content), i;
  for (i = 0; i + 1 < length; ++i) {
    if (in_string) {
      if (escaped)
        escaped = false;
      else if (content[i] == '\\')
        escaped = true;
      else if (content[i] == '"')
        in_string = false;
      continue;
    }
    if (content[i] == '"') {
      in_string = true;
      continue;
    }
    if (content[i] == '0' && isdigit((unsigned char)content[i + 1])) {
      size_t previous = i;
      while (previous > 0 &&
             (content[previous - 1] == ' ' || content[previous - 1] == '\t' ||
              content[previous - 1] == '\r' || content[previous - 1] == '\n'))
        --previous;
      if (previous == 0 || content[previous - 1] == ':' ||
          content[previous - 1] == ',' || content[previous - 1] == '[' ||
          content[previous - 1] == '-') {
        *position = i + 1;
        return 1;
      }
    }
  }
  return 0;
}

static int json_invalid_token_position(const char *content, size_t *position) {
  bool in_string = false, escaped = false;
  size_t i, length = strlen(content);
  for (i = 0; i < length; ++i) {
    unsigned char character = (unsigned char)content[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (content[i] == '\\') {
        escaped = true;
      } else if (content[i] == '"') {
        in_string = false;
      }
      continue;
    }
    if (content[i] == '"') {
      in_string = true;
      continue;
    }
    if (content[i] == '-' || isdigit(character)) {
      Scanner number = {content, i, length};
      if (skip_json_number(&number) && number.position > i)
        i = number.position - 1;
      continue;
    }
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z')))
      continue;
    if (content[i] == 't' || content[i] == 'f' || content[i] == 'n') {
      const char *literal = content[i] == 't'   ? "true"
                            : content[i] == 'f' ? "false"
                                                : "null";
      size_t j, literal_size = strlen(literal);
      for (j = 0; j < literal_size; ++j) {
        size_t current = i + j;
        if (current >= length || content[current] != literal[j]) {
          if (current < length) {
            *position = current;
            return 1;
          }
          return 0;
        }
      }
      i += literal_size - 1;
      if (content[i + 1] != '\0' && !json_value_terminator(content[i + 1])) {
        *position = i + 1;
        return 1;
      }
      continue;
    }
    *position = i;
    return 1;
  }
  return 0;
}

static int json_missing_value_position(const char *content, size_t *position) {
  bool in_string = false, escaped = false;
  size_t i, length = strlen(content);
  for (i = 0; i < length; ++i) {
    if (in_string) {
      if (escaped)
        escaped = false;
      else if (content[i] == '\\')
        escaped = true;
      else if (content[i] == '"')
        in_string = false;
      continue;
    }
    if (content[i] == '"') {
      in_string = true;
      continue;
    }
    if (content[i] == ':') {
      size_t next = i + 1;
      while (next < length && (content[next] == ' ' || content[next] == '\t' ||
                               content[next] == '\r' || content[next] == '\n'))
        ++next;
      if (next < length && (content[next] == '}' || content[next] == ']' ||
                            content[next] == ',')) {
        *position = next;
        return 1;
      }
    }
  }
  return 0;
}

static void json_unexpected_token_error(const char *content,
                                        size_t token_position, char *error,
                                        size_t error_size) {
  size_t length = strlen(content);
  size_t start = length > 20 && token_position > 10 ? token_position - 10 : 0;
  size_t end = length > 20 && length - token_position > 10 ? token_position + 10
                                                           : length;
  size_t excerpt_size = end - start;
  if (error != NULL && error_size > 0)
    snprintf(error, error_size,
             "Unexpected token '%c', %s\"%.*s\"%s is not valid JSON",
             content[token_position], start > 0 ? "..." : "", (int)excerpt_size,
             content + start, end < length ? "..." : "");
}

static void json_parse_error(const char *content, char *error,
                             size_t error_size) {
  Scanner scanner = {content, 0, strlen(content)};
  size_t position, line, column, invalid_position = 0;
  size_t object_trailing_position = 0, array_trailing_position = 0;
  size_t leading_zero_position = 0, missing_value_position = 0;
  bool has_invalid, has_object_trailing, has_array_trailing;
  bool has_leading_zero, has_missing_value;
  spaces(&scanner);
  if (scanner.position >= scanner.length) {
    set_error(error, error_size, "Unexpected end of JSON input");
    return;
  }
  if (json_empty_root_object(content, &position)) {
    json_position_to_line_column(content, position, &line, &column);
    if (error != NULL && error_size > 0)
      snprintf(error, error_size,
               "Expected property name or '}' in JSON at position %zu (line "
               "%zu column %zu)",
               position, line, column);
    return;
  }
  has_invalid = json_invalid_token_position(content, &invalid_position);
  has_object_trailing =
      json_trailing_comma_position(content, '}', &object_trailing_position);
  has_array_trailing =
      json_trailing_comma_position(content, ']', &array_trailing_position);
  has_leading_zero =
      json_leading_zero_position(content, &leading_zero_position);
  has_missing_value =
      json_missing_value_position(content, &missing_value_position);
  if (has_missing_value &&
      (!has_invalid || missing_value_position < invalid_position) &&
      (!has_object_trailing ||
       missing_value_position < object_trailing_position) &&
      (!has_array_trailing ||
       missing_value_position < array_trailing_position) &&
      (!has_leading_zero || missing_value_position < leading_zero_position)) {
    json_unexpected_token_error(content, missing_value_position, error,
                                error_size);
    return;
  }
  if (has_leading_zero &&
      (!has_invalid || leading_zero_position < invalid_position) &&
      (!has_object_trailing ||
       leading_zero_position < object_trailing_position) &&
      (!has_array_trailing ||
       leading_zero_position < array_trailing_position)) {
    json_position_to_line_column(content, leading_zero_position, &line,
                                 &column);
    if (error != NULL && error_size > 0)
      snprintf(error, error_size,
               "Unexpected number in JSON at position %zu (line %zu column "
               "%zu)",
               leading_zero_position, line, column);
    return;
  }
  if (has_object_trailing &&
      (!has_invalid || object_trailing_position < invalid_position) &&
      (!has_array_trailing ||
       object_trailing_position < array_trailing_position)) {
    json_position_to_line_column(content, object_trailing_position, &line,
                                 &column);
    if (error != NULL && error_size > 0)
      snprintf(error, error_size,
               "Expected double-quoted property name in JSON at position %zu "
               "(line %zu column %zu)",
               object_trailing_position, line, column);
    return;
  }
  if (has_array_trailing &&
      (!has_invalid || array_trailing_position < invalid_position)) {
    json_unexpected_token_error(content, array_trailing_position, error,
                                error_size);
    return;
  }
  if (has_invalid) {
    json_unexpected_token_error(content, invalid_position, error, error_size);
    return;
  }
  set_error(error, error_size, "JSON version file has no root version string");
}

static int json_fields(const char *content, const char *filename,
                       JsonFields *fields) {
  Scanner root = {content, 0, strlen(content)};
  Range packages;
  Range raw;
  memset(fields, 0, sizeof(*fields));
  if (!json_object_range(content, &fields->root_object))
    return 0;
  spaces(&root);
  fields->has_root_version =
      object_field(&root, "version", &fields->root_version, NULL, 0);
  if (!fields->has_root_version &&
      strstr(filename, "package-lock.json") == NULL &&
      strstr(filename, "npm-shrinkwrap.json") == NULL)
    return 0;
  root.position = 0;
  spaces(&root);
  if (object_field(&root, "private", &fields->private_value, NULL, 0)) {
    fields->has_private = true;
    fields->is_private =
        fields->private_value.end - fields->private_value.start == 4 &&
        strncmp(content + fields->private_value.start, "true", 4) == 0;
  }
  if (strstr(filename, "package-lock.json") == NULL &&
      strstr(filename, "npm-shrinkwrap.json") == NULL)
    return 1;
  root.position = 0;
  spaces(&root);
  if (!object_field(&root, "packages", &packages, NULL, 0))
    return 1;
  root.position = packages.start;
  spaces(&root);
  if (!object_field(&root, "", &raw, NULL, 0))
    return 1;
  if (content[raw.start] == '{') {
    fields->lock_package_object = raw;
    fields->has_lock_package = true;
    root.position = raw.start;
    spaces(&root);
    if (object_field(&root, "version", &fields->lock_package_version, NULL, 0))
      fields->has_lock_package_version = true;
  }
  root.position = 0;
  spaces(&root);
  if (object_field(&root, "version", &fields->lock_version, NULL, 0))
    fields->has_lock_version = true;
  return 1;
}

static int copy_json_string(const char *text, Range range, char *out,
                            size_t out_size) {
  size_t decoded_length;
  Scanner scanner = {text, range.start, range.end};
  return string_value(&scanner, out, out_size, &decoded_length, NULL) &&
         decoded_length == strlen(out);
}

typedef struct {
  char type;
  size_t amount, uses, weight;
} IndentStat;

typedef struct {
  Scanner scanner;
  const Range *versions;
  const Range *insert_version_objects;
  size_t version_count, insert_version_object_count;
  const char *replacement;
  bool replacement_is_null;
  char indent_char;
  size_t indent_size;
  const char *newline;
  size_t newline_size;
  CsemverBuffer *output;
} JsonPrinter;

typedef struct {
  Range value;
  char *key_text;
  size_t key_size;
  unsigned long array_index;
  size_t order;
  bool is_index, is_replacement;
} JsonProperty;

static int append_json_indent(JsonPrinter *printer, size_t depth) {
  size_t level, column;
  if (depth > 512)
    return 0;
  for (level = 0; level < depth; ++level)
    for (column = 0; column < printer->indent_size; ++column)
      if (!csemver_buffer_append(printer->output, &printer->indent_char, 1))
        return 0;
  return 1;
}

static int append_json_newline(JsonPrinter *printer) {
  return csemver_buffer_append(printer->output, printer->newline,
                               printer->newline_size);
}

static int read_json_hex4(const char *text, size_t length, size_t *position,
                          unsigned int *value) {
  size_t i;
  unsigned int result = 0;
  for (i = 0; i < 4; ++i) {
    unsigned char c;
    if (*position >= length)
      return 0;
    c = (unsigned char)text[(*position)++];
    if (c >= '0' && c <= '9')
      result = result * 16 + (unsigned int)(c - '0');
    else if (c >= 'a' && c <= 'f')
      result = result * 16 + (unsigned int)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      result = result * 16 + (unsigned int)(c - 'A' + 10);
    else
      return 0;
  }
  *value = result;
  return 1;
}

static int append_json_codepoint(CsemverBuffer *output, unsigned int value) {
  char bytes[4];
  size_t count;
  if (value == '"' || value == '\\') {
    bytes[0] = '\\';
    bytes[1] = (char)value;
    return csemver_buffer_append(output, bytes, 2);
  }
  if (value == '\b')
    return csemver_buffer_append(output, "\\b", 2);
  if (value == '\f')
    return csemver_buffer_append(output, "\\f", 2);
  if (value == '\n')
    return csemver_buffer_append(output, "\\n", 2);
  if (value == '\r')
    return csemver_buffer_append(output, "\\r", 2);
  if (value == '\t')
    return csemver_buffer_append(output, "\\t", 2);
  if (value < 0x20 || (value >= 0xd800 && value <= 0xdfff)) {
    char escaped[7];
    snprintf(escaped, sizeof escaped, "\\u%04x", value);
    return csemver_buffer_append(output, escaped, 6);
  }
  if (value <= 0x7f) {
    bytes[0] = (char)value;
    count = 1;
  } else if (value <= 0x7ff) {
    bytes[0] = (char)(0xc0 | (value >> 6));
    bytes[1] = (char)(0x80 | (value & 0x3f));
    count = 2;
  } else if (value <= 0xffff) {
    bytes[0] = (char)(0xe0 | (value >> 12));
    bytes[1] = (char)(0x80 | ((value >> 6) & 0x3f));
    bytes[2] = (char)(0x80 | (value & 0x3f));
    count = 3;
  } else if (value <= 0x10ffff) {
    bytes[0] = (char)(0xf0 | (value >> 18));
    bytes[1] = (char)(0x80 | ((value >> 12) & 0x3f));
    bytes[2] = (char)(0x80 | ((value >> 6) & 0x3f));
    bytes[3] = (char)(0x80 | (value & 0x3f));
    count = 4;
  } else
    return 0;
  return csemver_buffer_append(output, bytes, count);
}

static int json_print_string(Scanner *scanner, CsemverBuffer *output) {
  size_t position = scanner->position;
  if (position >= scanner->length || scanner->text[position++] != '"' ||
      !csemver_buffer_append(output, "\"", 1))
    return 0;
  while (position < scanner->length) {
    unsigned char c = (unsigned char)scanner->text[position++];
    unsigned int value = c;
    if (c == '"') {
      if (!csemver_buffer_append(output, "\"", 1))
        return 0;
      scanner->position = position;
      return 1;
    }
    if (c == '\\') {
      unsigned char escape;
      if (position >= scanner->length)
        return 0;
      escape = (unsigned char)scanner->text[position++];
      if (escape == 'u') {
        if (!read_json_hex4(scanner->text, scanner->length, &position, &value))
          return 0;
        if (value >= 0xd800 && value <= 0xdbff &&
            position + 6 <= scanner->length &&
            scanner->text[position] == '\\' &&
            scanner->text[position + 1] == 'u') {
          size_t low_position = position + 2;
          unsigned int low;
          if (read_json_hex4(scanner->text, scanner->length, &low_position,
                             &low) &&
              low >= 0xdc00 && low <= 0xdfff) {
            value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
            position = low_position;
          }
        }
      } else if (escape == '"' || escape == '\\' || escape == '/')
        value = escape;
      else if (escape == 'b')
        value = '\b';
      else if (escape == 'f')
        value = '\f';
      else if (escape == 'n')
        value = '\n';
      else if (escape == 'r')
        value = '\r';
      else if (escape == 't')
        value = '\t';
      else
        return 0;
      if (!append_json_codepoint(output, value))
        return 0;
    } else {
      if (c < 0x20 || !csemver_buffer_append(output, (const char *)&c, 1))
        return 0;
    }
  }
  return 0;
}

static void json_property_array_index(JsonProperty *property) {
  size_t position = 1, end, digits = 0;
  unsigned long value = 0;
  property->is_index = false;
  property->array_index = 0;
  if (property->key_size < 2 || property->key_text[0] != '"' ||
      property->key_text[property->key_size - 1] != '"')
    return;
  end = property->key_size - 1;
  while (position < end) {
    unsigned char c = (unsigned char)property->key_text[position++];
    if (c < '0' || c > '9' || (digits == 1 && value == 0))
      return;
    if (value > (4294967294UL - (unsigned long)(c - '0')) / 10UL)
      return;
    value = value * 10UL + (unsigned long)(c - '0');
    ++digits;
  }
  if (digits != 0) {
    property->is_index = true;
    property->array_index = value;
  }
}

static int json_property_compare(const void *left, const void *right) {
  const JsonProperty *a = left;
  const JsonProperty *b = right;
  if (a->is_index != b->is_index)
    return a->is_index ? -1 : 1;
  if (a->is_index && a->array_index != b->array_index)
    return a->array_index < b->array_index ? -1 : 1;
  return a->order < b->order ? -1 : a->order > b->order;
}

static int json_property_add(JsonProperty **properties, size_t *count,
                             size_t *capacity, JsonProperty property) {
  if (*count == *capacity) {
    size_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    JsonProperty *grown;
    if (new_capacity < *capacity ||
        new_capacity > (size_t)-1 / sizeof **properties)
      return 0;
    grown = realloc(*properties, new_capacity * sizeof **properties);
    if (grown == NULL)
      return 0;
    *properties = grown;
    *capacity = new_capacity;
  }
  (*properties)[(*count)++] = property;
  return 1;
}

static int indent_stat_index(IndentStat *stats, size_t count, char type,
                             size_t amount, size_t *index) {
  size_t i;
  for (i = 0; i < count; ++i)
    if (stats[i].type == type && stats[i].amount == amount) {
      *index = i;
      return 1;
    }
  *index = count;
  return 2;
}

static size_t collect_indent_stats(const char *text, size_t length,
                                   bool ignore_single, IndentStat *stats,
                                   size_t capacity) {
  size_t count = 0, previous_size = 0, previous_key = (size_t)-1;
  size_t line_start = 0;
  char previous_type = '\0';

  while (line_start <= length) {
    size_t line_end = line_start, indent = 0, difference, index;
    size_t use = 1, weight = 0;
    char type = '\0';
    int found;

    while (line_end < length && text[line_end] != '\n')
      ++line_end;
    if (line_end != line_start) {
      while (line_start + indent < line_end &&
             (text[line_start + indent] == ' ' ||
              text[line_start + indent] == '\t')) {
        if (type == '\0')
          type = text[line_start + indent];
        if (text[line_start + indent] != type)
          break;
        ++indent;
      }
      if (type == '\0') {
        previous_size = 0;
        previous_type = '\0';
      } else if (!(ignore_single && type == ' ' && indent == 1)) {
        if (type != previous_type)
          previous_size = 0;
        previous_type = type;
        difference = indent >= previous_size ? indent - previous_size
                                             : previous_size - indent;
        previous_size = indent;
        if (difference == 0) {
          use = 0;
          weight = 1;
          index = previous_key;
        } else {
          if (ignore_single && type == ' ' && difference == 1)
            goto next_line;
          found = indent_stat_index(stats, count, type, difference, &index);
          if (found == 0 || (found == 2 && count >= capacity))
            goto next_line;
          if (found == 2) {
            memset(&stats[count], 0, sizeof stats[count]);
            stats[count].type = type;
            stats[count].amount = difference;
            ++count;
          }
          previous_key = index;
        }
        if (index != (size_t)-1 && index < count) {
          stats[index].uses += use;
          stats[index].weight += weight;
        }
      }
    }
  next_line:
    if (line_end == length)
      break;
    line_start = line_end + 1;
  }
  return count;
}

static int detect_json_format(const char *text, char *indent_char,
                              size_t *indent_size, const char **newline,
                              size_t *newline_size) {
  size_t length = strlen(text), lines = 1, i, count, best = 0;
  size_t best_uses = 0, best_weight = 0;
  IndentStat *stats;
  bool crlf = false;
  size_t crlf_count = 0, lf_count = 0;

  for (i = 0; i < length; ++i) {
    if (text[i] == '\n') {
      ++lines;
      if (i != 0 && text[i - 1] == '\r')
        ++crlf_count;
      else
        ++lf_count;
    }
  }
  if (lines == (size_t)-1 || lines + 1 > (size_t)-1 / sizeof *stats)
    return 0;
  stats = calloc(lines + 1, sizeof *stats);
  if (stats == NULL)
    return 0;
  count = collect_indent_stats(text, length, true, stats, lines + 1);
  if (count == 0)
    count = collect_indent_stats(text, length, false, stats, lines + 1);
  for (i = 0; i < count; ++i)
    if (stats[i].uses > best_uses ||
        (stats[i].uses == best_uses && stats[i].weight > best_weight)) {
      best = i;
      best_uses = stats[i].uses;
      best_weight = stats[i].weight;
    }
  if (best_uses == 0) {
    *indent_char = ' ';
    *indent_size = 2;
  } else {
    *indent_char = stats[best].type;
    *indent_size = stats[best].amount > 10 ? 10 : stats[best].amount;
  }
  free(stats);
  crlf = crlf_count > lf_count;
  *newline = crlf ? "\r\n" : "\n";
  *newline_size = crlf ? 2 : 1;
  return 1;
}

static int json_print_value(JsonPrinter *printer, size_t depth);

static int json_print_object(JsonPrinter *printer, size_t depth) {
  Scanner *scanner = &printer->scanner;
  CsemverBuffer *output = printer->output;
  JsonProperty *properties = NULL;
  size_t count = 0, capacity = 0, close_position, i;
  size_t object_start = scanner->position;
  bool insert_version = false;

  for (i = 0; i < printer->insert_version_object_count; ++i)
    if (printer->insert_version_objects[i].start == object_start) {
      insert_version = true;
      break;
    }

  if (scanner->text[scanner->position++] != '{')
    return 0;
  spaces(scanner);
  if (scanner->position < scanner->length &&
      scanner->text[scanner->position] == '}') {
    close_position = scanner->position++;
    if (!insert_version)
      return csemver_buffer_append(output, "{}", 2);
  } else {
    for (;;) {
      JsonProperty property;
      CsemverBuffer key;
      size_t existing;
      memset(&property, 0, sizeof property);
      csemver_buffer_init(&key);
      if (!json_print_string(scanner, &key)) {
        csemver_buffer_free(&key);
        goto fail;
      }
      property.key_text = key.data;
      property.key_size = key.length;
      json_property_array_index(&property);
      spaces(scanner);
      if (scanner->position >= scanner->length ||
          scanner->text[scanner->position++] != ':') {
        free(property.key_text);
        goto fail;
      }
      spaces(scanner);
      if (!skip_value(scanner, &property.value)) {
        free(property.key_text);
        goto fail;
      }
      for (existing = 0; existing < count; ++existing)
        if (properties[existing].key_size == property.key_size &&
            memcmp(properties[existing].key_text, property.key_text,
                   property.key_size) == 0)
          break;
      if (existing < count) {
        properties[existing].value = property.value;
        free(property.key_text);
      } else {
        property.order = count;
        if (!json_property_add(&properties, &count, &capacity, property)) {
          free(property.key_text);
          goto fail;
        }
      }
      spaces(scanner);
      if (scanner->position < scanner->length &&
          scanner->text[scanner->position] == ',') {
        ++scanner->position;
        spaces(scanner);
        continue;
      }
      if (scanner->position >= scanner->length ||
          scanner->text[scanner->position] != '}')
        goto fail;
      close_position = scanner->position++;
      break;
    }
  }
  if (insert_version) {
    JsonProperty property;
    memset(&property, 0, sizeof property);
    property.key_text = malloc(sizeof "\"version\"");
    if (property.key_text == NULL)
      goto fail;
    memcpy(property.key_text, "\"version\"", sizeof "\"version\"");
    property.key_size = sizeof "\"version\"" - 1;
    property.order = count;
    property.is_replacement = true;
    if (!json_property_add(&properties, &count, &capacity, property)) {
      free(property.key_text);
      goto fail;
    }
  }
  qsort(properties, count, sizeof *properties, json_property_compare);
  if (!csemver_buffer_append(output, "{", 1) || !append_json_newline(printer))
    goto fail;
  for (i = 0; i < count; ++i) {
    if (!append_json_indent(printer, depth + 1) ||
        !csemver_buffer_append(output, properties[i].key_text,
                               properties[i].key_size) ||
        !csemver_buffer_append(output, ": ", 2))
      goto fail;
    if (properties[i].is_replacement) {
      if (printer->replacement_is_null) {
        if (!csemver_buffer_append(output, "null", 4))
          goto fail;
      } else if (!csemver_buffer_append(output, "\"", 1) ||
                 !csemver_buffer_append(output, printer->replacement,
                                        strlen(printer->replacement)) ||
                 !csemver_buffer_append(output, "\"", 1))
        goto fail;
    } else {
      scanner->position = properties[i].value.start;
      if (!json_print_value(printer, depth + 1))
        goto fail;
    }
    if (i + 1 < count) {
      if (!csemver_buffer_append(output, ",", 1) ||
          !append_json_newline(printer))
        goto fail;
    } else if (!append_json_newline(printer) ||
               !append_json_indent(printer, depth) ||
               !csemver_buffer_append(output, "}", 1))
      goto fail;
  }
  scanner->position = close_position + 1;
  for (i = 0; i < count; ++i)
    free(properties[i].key_text);
  free(properties);
  return 1;
fail:
  for (i = 0; i < count; ++i)
    free(properties[i].key_text);
  free(properties);
  return 0;
}

static int json_print_array(JsonPrinter *printer, size_t depth) {
  Scanner *scanner = &printer->scanner;
  CsemverBuffer *output = printer->output;

  if (scanner->text[scanner->position++] != '[' ||
      !csemver_buffer_append(output, "[", 1))
    return 0;
  spaces(scanner);
  if (scanner->position < scanner->length &&
      scanner->text[scanner->position] == ']') {
    ++scanner->position;
    return csemver_buffer_append(output, "]", 1);
  }
  if (!append_json_newline(printer))
    return 0;
  for (;;) {
    if (!append_json_indent(printer, depth + 1) ||
        !json_print_value(printer, depth + 1))
      return 0;
    spaces(scanner);
    if (scanner->position < scanner->length &&
        scanner->text[scanner->position] == ',') {
      ++scanner->position;
      if (!csemver_buffer_append(output, ",", 1) ||
          !append_json_newline(printer))
        return 0;
      spaces(scanner);
      continue;
    }
    if (scanner->position >= scanner->length ||
        scanner->text[scanner->position++] != ']' ||
        !append_json_newline(printer) || !append_json_indent(printer, depth) ||
        !csemver_buffer_append(output, "]", 1))
      return 0;
    return 1;
  }
}

static int json_print_number(JsonPrinter *printer) {
  Scanner *scanner = &printer->scanner;
  CsemverBuffer *output = printer->output;
  char candidate[64], digits[32];
  char *parsed_end;
  size_t digit_count = 0, leading = 0, trailing, i;
  size_t decimal_digits = 0;
  int precision, candidate_length;
  bool negative = false, after_decimal = false;
  long long explicit_exponent = 0, point_position, exponent;
  double value;
  Range token;
  const char *p;

  if (!skip_value(scanner, &token))
    return 0;
  value = strtod(scanner->text + token.start, &parsed_end);
  if (parsed_end != scanner->text + token.end)
    return 0;
  if (!isfinite(value))
    return csemver_buffer_append(output, "null", 4);
  if (value == 0.0)
    return csemver_buffer_append(output, "0", 1);
  for (precision = 1; precision <= 17; ++precision) {
    candidate_length =
        snprintf(candidate, sizeof candidate, "%.*g", precision, value);
    if (candidate_length < 0 || (size_t)candidate_length >= sizeof candidate)
      return 0;
    if (strtod(candidate, &parsed_end) == value && *parsed_end == '\0')
      break;
  }
  if (precision > 17)
    return 0;
  p = candidate;
  if (*p == '-') {
    negative = true;
    ++p;
  }
  while (*p != '\0' && *p != 'e' && *p != 'E') {
    if (*p == '.')
      after_decimal = true;
    else if (isdigit((unsigned char)*p)) {
      if (digit_count >= sizeof digits)
        return 0;
      digits[digit_count++] = *p;
      if (!after_decimal)
        ++decimal_digits;
    } else
      return 0;
    ++p;
  }
  if (*p == 'e' || *p == 'E') {
    char *end;
    explicit_exponent = strtoll(p + 1, &end, 10);
    if (*end != '\0')
      return 0;
  }
  while (leading < digit_count && digits[leading] == '0')
    ++leading;
  if (leading == digit_count)
    return csemver_buffer_append(output, "0", 1);
  point_position =
      (long long)decimal_digits + explicit_exponent - (long long)leading;
  trailing = digit_count;
  while (trailing > leading + 1 && digits[trailing - 1] == '0')
    --trailing;
  digit_count = trailing - leading;
  if (negative && !csemver_buffer_append(output, "-", 1))
    return 0;
  if (point_position >= (long long)digit_count && point_position <= 21) {
    if (!csemver_buffer_append(output, digits + leading, digit_count))
      return 0;
    for (i = digit_count; i < (size_t)point_position; ++i)
      if (!csemver_buffer_append(output, "0", 1))
        return 0;
    return 1;
  }
  if (point_position > 0 && point_position <= 21) {
    if (!csemver_buffer_append(output, digits + leading,
                               (size_t)point_position) ||
        !csemver_buffer_append(output, ".", 1))
      return 0;
    return csemver_buffer_append(output,
                                 digits + leading + (size_t)point_position,
                                 digit_count - (size_t)point_position);
  }
  if (point_position <= 0 && point_position > -6) {
    if (!csemver_buffer_append(output, "0.", 2))
      return 0;
    for (i = 0; i < (size_t)-point_position; ++i)
      if (!csemver_buffer_append(output, "0", 1))
        return 0;
    return csemver_buffer_append(output, digits + leading, digit_count);
  }
  exponent = point_position - 1;
  if (!csemver_buffer_append(output, digits + leading, 1))
    return 0;
  if (digit_count > 1 &&
      (!csemver_buffer_append(output, ".", 1) ||
       !csemver_buffer_append(output, digits + leading + 1, digit_count - 1)))
    return 0;
  if (!csemver_buffer_append(output, "e", 1))
    return 0;
  if (exponent >= 0) {
    if (!csemver_buffer_append(output, "+", 1))
      return 0;
  } else {
    if (!csemver_buffer_append(output, "-", 1))
      return 0;
    exponent = -exponent;
  }
  return csemver_buffer_appendf(output, "%lld", exponent);
}

static int json_print_value(JsonPrinter *printer, size_t depth) {
  Scanner *scanner = &printer->scanner;
  size_t start;
  size_t i;
  Range token;
  char first;

  spaces(scanner);
  if (scanner->position >= scanner->length || depth > 512)
    return 0;
  start = scanner->position;
  for (i = 0; i < printer->version_count; ++i)
    if (printer->versions[i].start == start) {
      if (!skip_value(scanner, NULL))
        return 0;
      if (printer->replacement_is_null)
        return csemver_buffer_append(printer->output, "null", 4);
      return csemver_buffer_append(printer->output, "\"", 1) &&
             csemver_buffer_append(printer->output, printer->replacement,
                                   strlen(printer->replacement)) &&
             csemver_buffer_append(printer->output, "\"", 1);
    }
  first = scanner->text[start];
  if (first == '{')
    return json_print_object(printer, depth);
  if (first == '[')
    return json_print_array(printer, depth);
  if (first == '"')
    return json_print_string(scanner, printer->output);
  if (first == '-' || isdigit((unsigned char)first))
    return json_print_number(printer);
  if (!skip_value(scanner, &token))
    return 0;
  return csemver_buffer_append(printer->output, scanner->text + token.start,
                               token.end - token.start);
}

static int json_update_formatted(const char *content, const Range *versions,
                                 size_t version_count, const char *new_version,
                                 bool replacement_is_null,
                                 const Range *insert_version_objects,
                                 size_t insert_version_object_count,
                                 char **updated, size_t *updated_size) {
  JsonPrinter printer;
  CsemverBuffer output;
  const char *newline;
  size_t newline_size;

  memset(&printer, 0, sizeof printer);
  if (!detect_json_format(content, &printer.indent_char, &printer.indent_size,
                          &newline, &newline_size))
    return 0;
  printer.scanner.text = content;
  printer.scanner.length = strlen(content);
  printer.versions = versions;
  printer.insert_version_objects = insert_version_objects;
  printer.insert_version_object_count = insert_version_object_count;
  printer.version_count = version_count;
  printer.replacement = new_version;
  printer.replacement_is_null = replacement_is_null;
  printer.newline = newline;
  printer.newline_size = newline_size;
  csemver_buffer_init(&output);
  printer.output = &output;
  spaces(&printer.scanner);
  if (!json_print_value(&printer, 0))
    goto fail;
  spaces(&printer.scanner);
  if (printer.scanner.position != printer.scanner.length ||
      !append_json_newline(&printer))
    goto fail;
  *updated = output.data;
  *updated_size = output.length;
  return 1;
fail:
  csemver_buffer_free(&output);
  return 0;
}

int csemver_json_repository_url(const char *content, char *url,
                                size_t url_size) {
  Scanner root;
  Range repository;
  int found;

  if (content == NULL || url == NULL || url_size == 0)
    return 0;
  url[0] = '\0';
  root.text = content;
  root.position = 0;
  root.length = strlen(content);
  spaces(&root);
  if (!object_field(&root, "repository", &repository, NULL, 0))
    return 0;
  root.position = repository.start;
  if (content[repository.start] == '"') {
    size_t decoded_length;
    found = string_value(&root, url, url_size, &decoded_length, NULL) &&
            decoded_length == strlen(url);
  } else if (content[repository.start] == '{')
    found = object_field(&root, "url", NULL, url, url_size);
  else
    found = 0;
  if (!found || url[0] == '\0') {
    url[0] = '\0';
    return 0;
  }
  return 1;
}

static int json_config_field(const char *content, const char *object_key,
                             const char *field_key, Range *range, char *decoded,
                             size_t decoded_size) {
  Scanner root;
  Range object;

  if (content == NULL || object_key == NULL || field_key == NULL)
    return 0;
  root.text = content;
  root.position = 0;
  root.length = strlen(content);
  spaces(&root);
  if (!object_field(&root, object_key, &object, NULL, 0) ||
      object.start >= root.length || content[object.start] != '{')
    return 0;
  root.position = object.start;
  root.length = object.end;
  return object_field(&root, field_key, range, decoded, decoded_size);
}

int csemver_json_object_string(const char *content, const char *object_key,
                               const char *field_key, char *value,
                               size_t value_size) {
  if (value == NULL || value_size == 0)
    return 0;
  value[0] = '\0';
  return json_config_field(content, object_key, field_key, NULL, value,
                           value_size);
}

static int json_config_nested_field(const char *content, const char *object_key,
                                    const char *nested_key,
                                    const char *field_key, Range *range,
                                    char *decoded, size_t decoded_size) {
  Range nested;
  Scanner object;

  if (!json_config_field(content, object_key, nested_key, &nested, NULL, 0) ||
      nested.start >= nested.end || content[nested.start] != '{')
    return 0;
  object.text = content;
  object.position = nested.start;
  object.length = nested.end;
  return object_field(&object, field_key, range, decoded, decoded_size);
}

int csemver_json_object_nested_string(const char *content,
                                      const char *object_key,
                                      const char *nested_key,
                                      const char *field_key, char *value,
                                      size_t value_size) {
  if (value == NULL || value_size == 0)
    return 0;
  value[0] = '\0';
  return json_config_nested_field(content, object_key, nested_key, field_key,
                                  NULL, value, value_size);
}

static int json_range_boolean(const char *content, Range field, bool *value) {
  size_t length = field.end - field.start;
  if (length == 4 && memcmp(content + field.start, "true", 4) == 0) {
    *value = true;
    return 1;
  }
  if (length == 5 && memcmp(content + field.start, "false", 5) == 0) {
    *value = false;
    return 1;
  }
  return 0;
}

int csemver_json_object_nested_boolean(const char *content,
                                       const char *object_key,
                                       const char *nested_key,
                                       const char *field_key, bool *value) {
  Range field;
  return value != NULL &&
         json_config_nested_field(content, object_key, nested_key, field_key,
                                  &field, NULL, 0) &&
         json_range_boolean(content, field, value);
}

int csemver_json_object_boolean(const char *content, const char *object_key,
                                const char *field_key, bool *value) {
  Range field;

  if (value == NULL ||
      !json_config_field(content, object_key, field_key, &field, NULL, 0))
    return 0;
  return json_range_boolean(content, field, value);
}

int csemver_json_object_string_array(const char *content,
                                     const char *object_key,
                                     const char *field_key, char *values,
                                     size_t value_stride, size_t max_values,
                                     size_t *value_count) {
  Range field;
  Scanner array;
  size_t count = 0;

  if (value_count != NULL)
    *value_count = 0;
  if (values == NULL || value_stride == 0 || value_count == NULL ||
      max_values > SIZE_MAX / value_stride ||
      !json_config_field(content, object_key, field_key, &field, NULL, 0) ||
      field.start >= field.end || content[field.start] != '[')
    return 0;
  array.text = content;
  array.position = field.start + 1;
  array.length = field.end;
  spaces(&array);
  while (array.position < array.length && array.text[array.position] != ']') {
    if (count >= max_values ||
        !string_value(&array, values + count * value_stride, value_stride, NULL,
                      NULL))
      return 0;
    ++count;
    spaces(&array);
    if (array.position >= array.length)
      return 0;
    if (array.text[array.position] == ',') {
      ++array.position;
      spaces(&array);
    } else if (array.text[array.position] != ']') {
      return 0;
    }
  }
  if (array.position >= array.length || array.text[array.position] != ']')
    return 0;
  ++array.position;
  spaces(&array);
  if (array.position != array.length)
    return 0;
  *value_count = count;
  return 1;
}

int csemver_json_object_unsigned(const char *content, const char *object_key,
                                 const char *field_key, unsigned *value) {
  Range field;
  unsigned number = 0;
  size_t position;

  if (value == NULL ||
      !json_config_field(content, object_key, field_key, &field, NULL, 0) ||
      field.start >= field.end)
    return 0;
  for (position = field.start; position < field.end; ++position) {
    unsigned char character = (unsigned char)content[position];
    unsigned digit;
    if (character < '0' || character > '9')
      return 0;
    digit = (unsigned)(character - '0');
    if (number > (UINT_MAX - digit) / 10)
      return 0;
    number = number * 10 + digit;
  }
  *value = number;
  return 1;
}

static int json_object_file_array(const char *content, const char *object_key,
                                  const char *field_key, char *filenames,
                                  size_t filename_stride, char *types,
                                  size_t type_stride, size_t max_values,
                                  size_t *file_count,
                                  int allow_string_entries) {
  Range field;
  Scanner array;
  size_t count = 0;

  if (file_count != NULL)
    *file_count = 0;
  if (filenames == NULL || filename_stride == 0 || types == NULL ||
      type_stride == 0 || file_count == NULL ||
      max_values > SIZE_MAX / filename_stride ||
      max_values > SIZE_MAX / type_stride ||
      !json_config_field(content, object_key, field_key, &field, NULL, 0) ||
      field.start >= field.end || content[field.start] != '[')
    return 0;
  array.text = content;
  array.position = field.start + 1;
  array.length = field.end;
  spaces(&array);
  while (array.position < array.length && array.text[array.position] != ']') {
    Range item;
    Range updater;
    Scanner object;
    char *filename;
    char *type;
    if (count >= max_values)
      return 0;
    filename = filenames + count * filename_stride;
    type = types + count * type_stride;
    memset(type, 0, type_stride);
    if (allow_string_entries && array.text[array.position] == '"') {
      if (!string_value(&array, filename, filename_stride, NULL, NULL))
        return 0;
    } else {
      if (array.text[array.position] != '{' || !skip_value(&array, &item))
        return 0;
      object.text = array.text;
      object.position = item.start;
      object.length = item.end;
      if (!object_field(&object, "filename", NULL, filename, filename_stride))
        return 0;
      object.position = item.start;
      if (!object_field(&object, "type", NULL, type, type_stride))
        return 0;
      object.position = item.start;
      if (object_field(&object, "updater", &updater, NULL, 0))
        return 0;
    }
    ++count;
    spaces(&array);
    if (array.position >= array.length)
      return 0;
    if (array.text[array.position] == ',') {
      ++array.position;
      spaces(&array);
    } else if (array.text[array.position] != ']') {
      return 0;
    }
  }
  if (array.position >= array.length || array.text[array.position] != ']')
    return 0;
  ++array.position;
  spaces(&array);
  if (array.position != array.length)
    return 0;
  *file_count = count;
  return 1;
}

int csemver_json_object_typed_file_array(const char *content,
                                         const char *object_key,
                                         const char *field_key, char *filenames,
                                         size_t filename_stride, char *types,
                                         size_t type_stride, size_t max_values,
                                         size_t *file_count) {
  return json_object_file_array(content, object_key, field_key, filenames,
                                filename_stride, types, type_stride, max_values,
                                file_count, 0);
}

int csemver_json_object_mixed_file_array(const char *content,
                                         const char *object_key,
                                         const char *field_key, char *filenames,
                                         size_t filename_stride, char *types,
                                         size_t type_stride, size_t max_values,
                                         size_t *file_count) {
  return json_object_file_array(content, object_key, field_key, filenames,
                                filename_stride, types, type_stride, max_values,
                                file_count, 1);
}

int csemver_json_object_commit_type_array(
    const char *content, const char *object_key, const char *field_key,
    char *types, size_t type_stride, char *sections, size_t section_stride,
    bool *hidden, bool *bump, size_t max_types, size_t *type_count) {
  Range field;
  Scanner array;
  size_t count = 0;

  if (type_count != NULL)
    *type_count = 0;
  if (types == NULL || type_stride == 0 || sections == NULL ||
      section_stride == 0 || hidden == NULL || bump == NULL ||
      type_count == NULL || max_types > SIZE_MAX / type_stride ||
      max_types > SIZE_MAX / section_stride ||
      !json_config_field(content, object_key, field_key, &field, NULL, 0) ||
      field.start >= field.end || content[field.start] != '[')
    return 0;
  array.text = content;
  array.position = field.start + 1;
  array.length = field.end;
  spaces(&array);
  while (array.position < array.length && array.text[array.position] != ']') {
    Range item;
    Range value;
    Scanner object;
    char *type;
    char *section;
    char effect[32] = {0};
    bool is_hidden = false;
    bool bumps = true;
    if (count >= max_types || array.text[array.position] != '{' ||
        !skip_value(&array, &item))
      return 0;
    type = types + count * type_stride;
    section = sections + count * section_stride;
    object.text = array.text;
    object.position = item.start;
    object.length = item.end;
    if (!object_field(&object, "type", NULL, type, type_stride))
      return 0;
    object.position = item.start;
    if (object_field(&object, "section", &value, NULL, 0)) {
      Scanner string = {array.text, value.start, value.end};
      if (!string_value(&string, section, section_stride, NULL, NULL))
        return 0;
      spaces(&string);
      if (string.position != value.end)
        return 0;
    } else
      section[0] = '\0';
    object.position = item.start;
    if (object_field(&object, "hidden", &value, NULL, 0)) {
      size_t length = value.end - value.start;
      if (length == 4 && memcmp(array.text + value.start, "true", 4) == 0)
        is_hidden = true;
      else if (length != 5 || memcmp(array.text + value.start, "false", 5) != 0)
        return 0;
    }
    object.position = item.start;
    if (object_field(&object, "effect", &value, NULL, 0)) {
      Scanner string = {array.text, value.start, value.end};
      if (!string_value(&string, effect, sizeof effect, NULL, NULL))
        return 0;
      spaces(&string);
      if (string.position != value.end)
        return 0;
      if (strcmp(effect, "hidden") == 0) {
        is_hidden = true;
        bumps = false;
      } else if (strcmp(effect, "changelog") == 0) {
        is_hidden = false;
        bumps = false;
      } else if (strcmp(effect, "bump") == 0) {
        is_hidden = false;
        bumps = true;
      } else {
        return 0;
      }
    } else
      bumps = !is_hidden;
    hidden[count] = is_hidden;
    bump[count] = bumps;
    ++count;
    spaces(&array);
    if (array.position >= array.length)
      return 0;
    if (array.text[array.position] == ',') {
      ++array.position;
      spaces(&array);
    } else if (array.text[array.position] != ']')
      return 0;
  }
  if (array.position >= array.length || array.text[array.position] != ']')
    return 0;
  ++array.position;
  spaces(&array);
  if (array.position != array.length)
    return 0;
  *type_count = count;
  return 1;
}

static int line_version(const char *content, const char *key, bool colon,
                        Range *range, char *version, size_t version_size) {
  const char *line = content;
  size_t key_len = strlen(key);
  while (*line != '\0') {
    const char *end = strchr(line, '\n');
    const char *limit = end == NULL ? line + strlen(line) : end;
    const char *p = line;
    while (p < limit && (*p == ' ' || *p == '\t'))
      ++p;
    if ((size_t)(limit - p) >= key_len && strncmp(p, key, key_len) == 0) {
      const char *start = p + key_len;
      const char *finish = limit;
      if (!colon) {
        while (start < limit && (*start == ' ' || *start == '\t'))
          ++start;
        if (start == limit || *start++ != '=')
          goto next_line;
      }
      while (start < limit && (*start == ' ' || *start == '\t'))
        ++start;
      while (finish > start &&
             (finish[-1] == '\r' || finish[-1] == ' ' || finish[-1] == '\t'))
        --finish;
      if (start < finish && (*start == '"' || *start == '\'')) {
        char quote = *start++;
        if (finish <= start || finish[-1] != quote)
          goto next_line;
        --finish;
      }
      if (start == finish || (size_t)(finish - start) >= version_size)
        goto next_line;
      memcpy(version, start, (size_t)(finish - start));
      version[finish - start] = '\0';
      if (range != NULL) {
        range->start = (size_t)(start - content);
        range->end = (size_t)(finish - content);
      }
      return 1;
    }
  next_line:
    if (end == NULL)
      break;
    line = end + 1;
  }
  return 0;
}

static int gradle_version_range(const char *content, Range *range,
                                char *version, size_t version_size) {
  const char *line = content;
  while (*line != '\0') {
    const char *end = strpbrk(line, "\r\n");
    const char *limit = end == NULL ? line + strlen(line) : end;
    const char *cursor = line;
    if ((size_t)(limit - line) >= sizeof "version" - 1 &&
        memcmp(line, "version", sizeof "version" - 1) == 0) {
      const char *value_start;
      const char *last_quote = NULL;
      const char *scan;
      cursor += sizeof "version" - 1;
      while (cursor < limit && isspace((unsigned char)*cursor))
        ++cursor;
      if (cursor < limit && *cursor == '=') {
        ++cursor;
        while (cursor < limit && isspace((unsigned char)*cursor))
          ++cursor;
        if (cursor < limit && (*cursor == '\'' || *cursor == '"')) {
          value_start = cursor + 1;
          for (scan = value_start; scan < limit; ++scan)
            if (*scan == '\'' || *scan == '"')
              last_quote = scan;
          if (last_quote != NULL && last_quote > value_start &&
              ((*value_start >= '0' && *value_start <= '9') ||
               *value_start == '.')) {
            size_t version_length = (size_t)(last_quote - value_start);
            if (version != NULL) {
              if (version_length >= version_size)
                return 0;
              memcpy(version, value_start, version_length);
              version[version_length] = '\0';
            }
            if (range != NULL) {
              range->start = (size_t)(line - content);
              range->end = (size_t)(last_quote + 1 - content);
            }
            return 1;
          }
        }
      }
    }
    if (end == NULL)
      break;
    line = end + 1;
    if (*end == '\r' && *line == '\n')
      ++line;
  }
  return 0;
}

int csemver_version_read_text(const char *filename, const char *type,
                              const char *content, char *version,
                              size_t version_size, bool *is_private,
                              char *error, size_t error_size) {
  const char *kind =
      type != NULL && type[0] != '\0'
          ? type
          : (strstr(filename, ".json") != NULL            ? "json"
             : strstr(filename, "pyproject.toml") != NULL ? "python"
             : strstr(filename, ".toml") != NULL          ? "toml"
             : strstr(filename, "build.gradle") != NULL   ? "gradle"
             : strstr(filename, ".yaml") != NULL ||
                     strstr(filename, ".yml") != NULL
                 ? "yaml"
                 : "plain-text");
  if (is_private != NULL)
    *is_private = false;
  if (strcmp(kind, "json") == 0) {
    JsonFields fields;
    if (!json_fields(content, filename, &fields)) {
      json_parse_error(content, error, error_size);
      return 0;
    }
    if (!fields.has_root_version &&
        (strstr(filename, "package-lock.json") != NULL ||
         strstr(filename, "npm-shrinkwrap.json") != NULL)) {
      if (version_size < sizeof "undefined") {
        set_error(error, error_size, "version output buffer too small");
        return 0;
      }
      memcpy(version, "undefined", sizeof "undefined");
      return 1;
    }
    if (!copy_json_string(content, fields.root_version, version,
                          version_size)) {
      set_error(error, error_size,
                "JSON version file has no root version string");
      return 0;
    }
    if (is_private != NULL)
      *is_private = fields.is_private;
    return 1;
  }
  if (strcmp(kind, "gradle") == 0) {
    if (gradle_version_range(content, NULL, version, version_size))
      return 1;
    set_error(error, error_size,
              "Failed to read the version field in your gradle file - is it "
              "present?");
    return 0;
  }
  if ((strcmp(kind, "python") == 0 || strcmp(kind, "toml") == 0) &&
      line_version(content, "version", false, NULL, version, version_size))
    return 1;
  if ((strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) &&
      line_version(content, "version:", true, NULL, version, version_size))
    return 1;
  if (strcmp(kind, "plain-text") == 0) {
    size_t length = strcspn(content, "\r\n");
    if (length != 0 && length < version_size && content[length] == '\0') {
      memcpy(version, content, length + 1);
      return 1;
    }
    if (length != 0 && length < version_size && content[length] == '\n' &&
        content[length + 1] == '\0') {
      memcpy(version, content, length);
      version[length] = '\0';
      return 1;
    }
    if (length != 0 && length < version_size && content[length] == '\r' &&
        content[length + 1] == '\n' && content[length + 2] == '\0') {
      memcpy(version, content, length);
      version[length] = '\0';
      return 1;
    }
  }
  set_error(error, error_size, "unsupported or unreadable version file format");
  return 0;
}

int csemver_version_update_text(const char *filename, const char *type,
                                const char *content, const char *new_version,
                                char **updated, size_t *updated_size,
                                char *old_version, size_t old_version_size,
                                char *error, size_t error_size) {
  const char *kind =
      type != NULL && type[0] != '\0'
          ? type
          : (strstr(filename, ".json") != NULL            ? "json"
             : strstr(filename, "pyproject.toml") != NULL ? "python"
             : strstr(filename, ".toml") != NULL          ? "toml"
             : strstr(filename, "build.gradle") != NULL   ? "gradle"
             : strstr(filename, ".yaml") != NULL ||
                     strstr(filename, ".yml") != NULL
                 ? "yaml"
                 : "plain-text");
  const bool replacement_is_null = new_version == NULL;
  const char *replacement = replacement_is_null ? "null" : new_version;
  Range ranges[3];
  Range insertions[2];
  size_t count = 0, insertion_count = 0, i, j, pos = 0,
         length = strlen(content);
  CsemverBuffer buffer;
  if (!csemver_version_read_text(filename, type, content, old_version,
                                 old_version_size, NULL, error, error_size))
    return 0;
  if (strcmp(kind, "gradle") == 0) {
    static const char prefix[] = "version = \"";
    Range gradle_range;
    CsemverBuffer gradle_buffer;
    if (!gradle_version_range(content, &gradle_range, NULL, 0))
      goto bad_format;
    csemver_buffer_init(&gradle_buffer);
    if (!csemver_buffer_append(&gradle_buffer, content, gradle_range.start) ||
        !csemver_buffer_append(&gradle_buffer, prefix, sizeof prefix - 1) ||
        !csemver_buffer_append(&gradle_buffer, replacement,
                               strlen(replacement)) ||
        !csemver_buffer_append(&gradle_buffer, "\"", 1) ||
        !csemver_buffer_append(&gradle_buffer, content + gradle_range.end,
                               length - gradle_range.end) ||
        !csemver_buffer_append(&gradle_buffer, "", 0)) {
      csemver_buffer_free(&gradle_buffer);
      set_error(error, error_size, "out of memory updating version file");
      return 0;
    }
    *updated = gradle_buffer.data;
    *updated_size = gradle_buffer.length;
    return 1;
  }
  if (strcmp(kind, "json") == 0) {
    JsonFields fields;
    if (!json_fields(content, filename, &fields))
      goto bad_format;
    if (fields.has_root_version)
      ranges[count++] = fields.root_version;
    else
      insertions[insertion_count++] = fields.root_object;
    if (fields.has_lock_package_version)
      ranges[count++] = fields.lock_package_version;
    else if (fields.has_lock_package)
      insertions[insertion_count++] = fields.lock_package_object;
    if (!json_update_formatted(content, ranges, count, replacement,
                               replacement_is_null, insertions, insertion_count,
                               updated, updated_size)) {
      set_error(error, error_size,
                "malformed JSON or out of memory updating version file");
      return 0;
    }
    return 1;
  }
  if (strcmp(kind, "python") == 0 || strcmp(kind, "toml") == 0) {
    if (!line_version(content, "version", false, &ranges[count], old_version,
                      old_version_size))
      goto bad_format;
    ++count;
  } else if (strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) {
    if (!line_version(content, "version:", true, &ranges[count], old_version,
                      old_version_size))
      goto bad_format;
    ++count;
  } else if (strcmp(kind, "plain-text") == 0) {
    ranges[count++] = (Range){0, length};
  } else
    goto bad_format;
  for (i = 0; i < count; ++i)
    for (j = i + 1; j < count; ++j)
      if (ranges[j].start < ranges[i].start) {
        Range tmp = ranges[i];
        ranges[i] = ranges[j];
        ranges[j] = tmp;
      }
  csemver_buffer_init(&buffer);
  for (i = 0; i < count; ++i) {
    if (ranges[i].start < pos || ranges[i].end > length ||
        !csemver_buffer_append(&buffer, content + pos, ranges[i].start - pos))
      goto allocation_error;
    if (!csemver_buffer_append(&buffer, replacement, strlen(replacement)))
      goto allocation_error;
    pos = ranges[i].end;
  }
  if (!csemver_buffer_append(&buffer, content + pos, length - pos))
    goto allocation_error;
  *updated = buffer.data;
  *updated_size = buffer.length;
  return 1;
allocation_error:
  csemver_buffer_free(&buffer);
  set_error(error, error_size, "out of memory updating version file");
  return 0;
bad_format:
  set_error(error, error_size, "unsupported or malformed version file format");
  return 0;
}
