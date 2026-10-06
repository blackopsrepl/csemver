#include "version.h"

#include "common.h"
#include "maven.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

typedef struct {
  size_t start, end;
} Range;
typedef struct {
  bool is_mapping;
  bool is_root_mapping;
  bool is_info_mapping;
  bool expect_key;
  bool key_is_info;
  bool key_is_version;
  bool saw_version_key;
  bool closes_as_mapping_key;
} YamlFrame;
typedef struct {
  char *name;
  size_t name_length;
  char *value;
  size_t value_length;
} YamlScalarAnchor;
typedef struct {
  size_t output_start;
} YamlFlowFrame;
typedef struct {
  size_t start, end, next_start;
  size_t comment_start, comment_length;
  bool comment_on_new_line;
} YamlFlowSeparator;
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
  bool replacement_is_null, compact;
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
  if (printer->compact)
    return 1;
  if (depth > 512)
    return 0;
  for (level = 0; level < depth; ++level)
    for (column = 0; column < printer->indent_size; ++column)
      if (!csemver_buffer_append(printer->output, &printer->indent_char, 1))
        return 0;
  return 1;
}

static int append_json_newline(JsonPrinter *printer) {
  if (printer->compact)
    return 1;
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
        !(printer->compact ? csemver_buffer_append(output, ":", 1)
                           : csemver_buffer_append(output, ": ", 2)))
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

static int yaml_scalar_equals(const yaml_event_t *event, const char *value) {
  size_t value_size = strlen(value);
  return event->data.scalar.length == value_size &&
         memcmp(event->data.scalar.value, value, value_size) == 0;
}

static int yaml_mark_to_byte_offset(const char *content, size_t character_index,
                                    size_t *byte_offset) {
  size_t characters = 0;
  size_t bytes = 0;
  while (characters < character_index) {
    unsigned char first = (unsigned char)content[bytes];
    if (first == '\0')
      return 0;
    if (first < 0x80)
      ++bytes;
    else if ((first & 0xe0) == 0xc0)
      bytes += 2;
    else if ((first & 0xf0) == 0xe0)
      bytes += 3;
    else if ((first & 0xf8) == 0xf0)
      bytes += 4;
    else
      return 0;
    ++characters;
  }
  *byte_offset = bytes;
  return 1;
}

static int yaml_scalar_value_range(const char *content,
                                   const yaml_event_t *event, Range *range,
                                   char *version, size_t version_size) {
  size_t start;
  size_t end;
  size_t value_length = event->data.scalar.length;
  yaml_scalar_style_t style = event->data.scalar.style;
  if (!yaml_mark_to_byte_offset(content, event->start_mark.index, &start) ||
      !yaml_mark_to_byte_offset(content, event->end_mark.index, &end) ||
      start > end)
    return 0;
  while (start < end && (content[start] == '!' || content[start] == '&')) {
    while (start < end && content[start] != ' ' && content[start] != '\t' &&
           content[start] != '\r' && content[start] != '\n')
      ++start;
    while (start < end && (content[start] == ' ' || content[start] == '\t'))
      ++start;
  }
  if (style == YAML_SINGLE_QUOTED_SCALAR_STYLE ||
      style == YAML_DOUBLE_QUOTED_SCALAR_STYLE) {
    char quote;
    if (start >= end || (content[start] != '\'' && content[start] != '"'))
      return 0;
    quote = content[start++];
    if (end <= start || content[end - 1] != quote)
      return 0;
    --end;
  } else if (style == YAML_LITERAL_SCALAR_STYLE ||
             style == YAML_FOLDED_SCALAR_STYLE) {
    size_t header_end = start;
    while (header_end < end && content[header_end] != '\n' &&
           content[header_end] != '\r')
      ++header_end;
    if (header_end == end)
      return 0;
    start = header_end + 1;
    if (content[header_end] == '\r' && start < end && content[start] == '\n')
      ++start;
    while (start < end && (content[start] == ' ' || content[start] == '\t'))
      ++start;
    while (end > start &&
           (content[end - 1] == '\n' || content[end - 1] == '\r'))
      --end;
  }
  if (start > end)
    return 0;
  if (version != NULL) {
    if (value_length >= version_size)
      return 0;
    if (value_length != 0)
      memcpy(version, event->data.scalar.value, value_length);
    version[value_length] = '\0';
  }
  if (range != NULL) {
    range->start = start;
    range->end = end;
  }
  return 1;
}

static void yaml_scalar_anchors_free(YamlScalarAnchor *anchors,
                                     size_t anchor_count) {
  size_t index;
  for (index = 0; index < anchor_count; ++index) {
    free(anchors[index].name);
    free(anchors[index].value);
  }
  free(anchors);
}

static int yaml_scalar_anchor_store(YamlScalarAnchor **anchors,
                                    size_t *anchor_count,
                                    size_t *anchor_capacity,
                                    const yaml_event_t *event) {
  const char *anchor = (const char *)event->data.scalar.anchor;
  size_t anchor_length = strlen(anchor);
  size_t value_length = event->data.scalar.length;
  size_t index;
  char *name_copy = malloc(anchor_length + 1);
  char *value_copy = malloc(value_length + 1);
  if (name_copy == NULL || value_copy == NULL) {
    free(name_copy);
    free(value_copy);
    return 0;
  }
  memcpy(name_copy, anchor, anchor_length + 1);
  if (value_length != 0)
    memcpy(value_copy, event->data.scalar.value, value_length);
  value_copy[value_length] = '\0';
  for (index = 0; index < *anchor_count; ++index) {
    if ((*anchors)[index].name_length == anchor_length &&
        memcmp((*anchors)[index].name, anchor, anchor_length) == 0)
      break;
  }
  if (index == *anchor_count) {
    if (*anchor_count == *anchor_capacity) {
      size_t new_capacity = *anchor_capacity == 0 ? 8 : *anchor_capacity * 2;
      YamlScalarAnchor *new_anchors;
      if (new_capacity < *anchor_capacity ||
          new_capacity > SIZE_MAX / sizeof **anchors) {
        free(name_copy);
        free(value_copy);
        return 0;
      }
      new_anchors = realloc(*anchors, new_capacity * sizeof **anchors);
      if (new_anchors == NULL) {
        free(name_copy);
        free(value_copy);
        return 0;
      }
      *anchors = new_anchors;
      *anchor_capacity = new_capacity;
    }
    ++*anchor_count;
  } else {
    free((*anchors)[index].name);
    free((*anchors)[index].value);
  }
  (*anchors)[index] = (YamlScalarAnchor){
      .name = name_copy,
      .name_length = anchor_length,
      .value = value_copy,
      .value_length = value_length,
  };
  return 1;
}

static const YamlScalarAnchor *
yaml_scalar_anchor_find(const YamlScalarAnchor *anchors, size_t anchor_count,
                        const yaml_char_t *name) {
  size_t name_length = strlen((const char *)name);
  size_t index;
  for (index = anchor_count; index > 0; --index) {
    const YamlScalarAnchor *anchor = &anchors[index - 1];
    if (anchor->name_length == name_length &&
        memcmp(anchor->name, name, name_length) == 0)
      return anchor;
  }
  return NULL;
}

static int yaml_version_range(const char *content, bool openapi, Range *range,
                              char *version, size_t version_size,
                              size_t *scalar_end,
                              yaml_scalar_style_t *scalar_style,
                              bool *duplicate_version_key) {
  yaml_parser_t parser;
  YamlFrame *frames = NULL;
  YamlScalarAnchor *anchors = NULL;
  size_t anchor_count = 0;
  size_t anchor_capacity = 0;
  size_t depth = 0;
  size_t capacity = 0;
  bool found = false;
  bool failed = false;
  if (duplicate_version_key != NULL)
    *duplicate_version_key = false;
  if (!yaml_parser_initialize(&parser))
    return 0;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content,
                               strlen(content));
  for (;;) {
    yaml_event_t event;
    bool stop;
    if (!yaml_parser_parse(&parser, &event)) {
      failed = true;
      break;
    }
    stop = event.type == YAML_STREAM_END_EVENT ||
           event.type == YAML_DOCUMENT_END_EVENT;
    switch (event.type) {
    case YAML_MAPPING_START_EVENT:
    case YAML_SEQUENCE_START_EVENT: {
      bool is_mapping = event.type == YAML_MAPPING_START_EVENT;
      bool is_root_mapping = is_mapping && depth == 0;
      bool is_info_mapping =
          is_mapping && openapi && depth > 0 && frames[depth - 1].is_mapping &&
          frames[depth - 1].is_root_mapping && !frames[depth - 1].expect_key &&
          frames[depth - 1].key_is_info;
      bool closes_as_mapping_key = depth > 0 && frames[depth - 1].is_mapping &&
                                   frames[depth - 1].expect_key;
      if (depth > 0 && frames[depth - 1].is_mapping &&
          !frames[depth - 1].expect_key) {
        frames[depth - 1].expect_key = true;
        frames[depth - 1].key_is_info = false;
        frames[depth - 1].key_is_version = false;
      }
      if (depth == capacity) {
        size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
        YamlFrame *new_frames;
        if (new_capacity < capacity ||
            new_capacity > SIZE_MAX / sizeof *frames) {
          failed = true;
          break;
        }
        new_frames = realloc(frames, new_capacity * sizeof *frames);
        if (new_frames == NULL) {
          failed = true;
          break;
        }
        frames = new_frames;
        capacity = new_capacity;
      }
      frames[depth] = (YamlFrame){
          .is_mapping = is_mapping,
          .is_root_mapping = is_root_mapping,
          .is_info_mapping = is_info_mapping,
          .expect_key = true,
          .closes_as_mapping_key = closes_as_mapping_key,
      };
      ++depth;
      break;
    }
    case YAML_MAPPING_END_EVENT:
    case YAML_SEQUENCE_END_EVENT:
      if (depth > 0) {
        bool closes_as_mapping_key = frames[depth - 1].closes_as_mapping_key;
        --depth;
        if (closes_as_mapping_key && depth > 0 &&
            frames[depth - 1].is_mapping) {
          frames[depth - 1].expect_key = false;
          frames[depth - 1].key_is_info = false;
          frames[depth - 1].key_is_version = false;
        }
      }
      break;
    case YAML_SCALAR_EVENT:
      if (event.data.scalar.anchor != NULL &&
          !yaml_scalar_anchor_store(&anchors, &anchor_count, &anchor_capacity,
                                    &event))
        failed = true;
      if (depth > 0 && frames[depth - 1].is_mapping) {
        YamlFrame *frame = &frames[depth - 1];
        if (frame->expect_key) {
          frame->key_is_info = openapi && frame->is_root_mapping &&
                               yaml_scalar_equals(&event, "info");
          frame->key_is_version = yaml_scalar_equals(&event, "version") &&
                                  ((openapi && frame->is_info_mapping) ||
                                   (!openapi && frame->is_root_mapping));
          if (frame->key_is_version) {
            if (frame->saw_version_key && duplicate_version_key != NULL)
              *duplicate_version_key = true;
            frame->saw_version_key = true;
          }
          frame->expect_key = false;
        } else {
          if (frame->key_is_version && !found) {
            if (!yaml_scalar_value_range(content, &event, range, version,
                                         version_size) ||
                (scalar_end != NULL &&
                 !yaml_mark_to_byte_offset(content, event.end_mark.index,
                                           scalar_end))) {
              failed = true;
            } else {
              if (scalar_style != NULL)
                *scalar_style = event.data.scalar.style;
              if (range != NULL &&
                  event.data.scalar.style == YAML_PLAIN_SCALAR_STYLE) {
                size_t trailing = range->end;
                while (content[trailing] == ' ' || content[trailing] == '\t')
                  ++trailing;
                if (content[trailing] == '\0' || content[trailing] == '\r' ||
                    content[trailing] == '\n')
                  range->end = trailing;
              }
              found = true;
            }
          }
          frame->expect_key = true;
          frame->key_is_info = false;
          frame->key_is_version = false;
        }
      }
      break;
    case YAML_ALIAS_EVENT:
      if (depth > 0 && frames[depth - 1].is_mapping) {
        YamlFrame *frame = &frames[depth - 1];
        if (frame->expect_key) {
          frame->key_is_info = false;
          frame->key_is_version = false;
          frame->expect_key = false;
        } else {
          if (frame->key_is_version && !found &&
              event.data.alias.anchor != NULL) {
            const YamlScalarAnchor *anchor = yaml_scalar_anchor_find(
                anchors, anchor_count, event.data.alias.anchor);
            size_t start;
            size_t end;
            if (anchor != NULL &&
                (version == NULL || anchor->value_length < version_size) &&
                (range == NULL ||
                 (yaml_mark_to_byte_offset(content, event.start_mark.index,
                                           &start) &&
                  yaml_mark_to_byte_offset(content, event.end_mark.index,
                                           &end)))) {
              if (version != NULL) {
                if (anchor->value_length != 0)
                  memcpy(version, anchor->value, anchor->value_length);
                version[anchor->value_length] = '\0';
              }
              if (range != NULL) {
                range->start = start;
                range->end = end;
                while (content[range->end] == ' ' ||
                       content[range->end] == '\t')
                  ++range->end;
                if (content[range->end] != '\0' &&
                    content[range->end] != '\r' && content[range->end] != '\n')
                  range->end = end;
              }
              if (scalar_end != NULL &&
                  !yaml_mark_to_byte_offset(content, event.end_mark.index,
                                            scalar_end))
                failed = true;
              if (scalar_style != NULL)
                *scalar_style = YAML_PLAIN_SCALAR_STYLE;
              found = !failed;
            }
          }
          frame->expect_key = true;
          frame->key_is_info = false;
          frame->key_is_version = false;
        }
      }
      break;
    default:
      break;
    }
    yaml_event_delete(&event);
    if (failed || stop)
      break;
  }
  yaml_parser_delete(&parser);
  free(frames);
  yaml_scalar_anchors_free(anchors, anchor_count);
  return found && !failed;
}

static int yaml_has_multiple_documents(const char *content) {
  yaml_parser_t parser;
  size_t documents = 0;
  int status = 0;
  if (!yaml_parser_initialize(&parser))
    return -1;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content,
                               strlen(content));
  for (;;) {
    yaml_event_t event;
    bool stream_end;
    if (!yaml_parser_parse(&parser, &event)) {
      status = -1;
      break;
    }
    if (event.type == YAML_DOCUMENT_START_EVENT && ++documents > 1)
      status = 1;
    stream_end = event.type == YAML_STREAM_END_EVENT;
    yaml_event_delete(&event);
    if (status != 0 || stream_end)
      break;
  }
  yaml_parser_delete(&parser);
  return status;
}

static bool yaml_bare_cr_precedes_key(const char *content, size_t key_start) {
  while (key_start > 0 &&
         (content[key_start - 1] == ' ' || content[key_start - 1] == '\t'))
    --key_start;
  return key_start > 0 && content[key_start - 1] == '\r' &&
         (key_start < 2 || content[key_start - 2] != '\n');
}

static bool yaml_block_scalar_has_bare_cr(const char *content, size_t start,
                                          size_t end) {
  size_t position;
  for (position = start; position < end; ++position)
    if (content[position] == '\r' &&
        (position + 1 == end || content[position + 1] != '\n'))
      return true;
  return false;
}

static int yaml_has_bare_cr_stringifier_error(const char *content) {
  yaml_parser_t parser;
  YamlFrame *frames = NULL;
  size_t depth = 0;
  size_t capacity = 0;
  bool stringifier_error = false;
  bool failed = false;
  if (!yaml_parser_initialize(&parser))
    return -1;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content,
                               strlen(content));
  for (;;) {
    yaml_event_t event;
    bool stream_end;
    if (!yaml_parser_parse(&parser, &event)) {
      failed = true;
      break;
    }
    switch (event.type) {
    case YAML_MAPPING_START_EVENT:
    case YAML_SEQUENCE_START_EVENT: {
      bool is_mapping = event.type == YAML_MAPPING_START_EVENT;
      bool closes_as_mapping_key = depth > 0 && frames[depth - 1].is_mapping &&
                                   frames[depth - 1].expect_key;
      if (depth > 0 && frames[depth - 1].is_mapping &&
          !frames[depth - 1].expect_key)
        frames[depth - 1].expect_key = true;
      if (depth == capacity) {
        size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
        YamlFrame *new_frames;
        if (new_capacity < capacity ||
            new_capacity > SIZE_MAX / sizeof *frames) {
          failed = true;
          break;
        }
        new_frames = realloc(frames, new_capacity * sizeof *frames);
        if (new_frames == NULL) {
          failed = true;
          break;
        }
        frames = new_frames;
        capacity = new_capacity;
      }
      frames[depth++] = (YamlFrame){
          .is_mapping = is_mapping,
          .expect_key = true,
          .closes_as_mapping_key = closes_as_mapping_key,
      };
      break;
    }
    case YAML_MAPPING_END_EVENT:
    case YAML_SEQUENCE_END_EVENT:
      if (depth > 0) {
        bool closes_as_mapping_key = frames[depth - 1].closes_as_mapping_key;
        --depth;
        if (closes_as_mapping_key && depth > 0 && frames[depth - 1].is_mapping)
          frames[depth - 1].expect_key = false;
      }
      break;
    case YAML_SCALAR_EVENT:
      if (event.data.scalar.style == YAML_LITERAL_SCALAR_STYLE ||
          event.data.scalar.style == YAML_FOLDED_SCALAR_STYLE) {
        size_t start;
        size_t end;
        if (!yaml_mark_to_byte_offset(content, event.start_mark.index,
                                      &start) ||
            !yaml_mark_to_byte_offset(content, event.end_mark.index, &end)) {
          failed = true;
        } else if (yaml_block_scalar_has_bare_cr(content, start, end)) {
          stringifier_error = true;
        }
      }
      if (depth > 0 && frames[depth - 1].is_mapping) {
        YamlFrame *frame = &frames[depth - 1];
        if (frame->expect_key) {
          size_t key_start;
          if (!yaml_mark_to_byte_offset(content, event.start_mark.index,
                                        &key_start))
            failed = true;
          else if (yaml_bare_cr_precedes_key(content, key_start))
            stringifier_error = true;
          frame->expect_key = false;
        } else {
          frame->expect_key = true;
        }
      }
      break;
    case YAML_ALIAS_EVENT:
      if (depth > 0 && frames[depth - 1].is_mapping) {
        YamlFrame *frame = &frames[depth - 1];
        if (frame->expect_key) {
          size_t key_start;
          if (!yaml_mark_to_byte_offset(content, event.start_mark.index,
                                        &key_start))
            failed = true;
          else if (yaml_bare_cr_precedes_key(content, key_start))
            stringifier_error = true;
          frame->expect_key = false;
        } else {
          frame->expect_key = true;
        }
      }
      break;
    default:
      break;
    }
    stream_end = event.type == YAML_STREAM_END_EVENT;
    yaml_event_delete(&event);
    if (failed || stringifier_error || stream_end)
      break;
  }
  yaml_parser_delete(&parser);
  free(frames);
  return stringifier_error ? 1 : (failed ? -1 : 0);
}

static size_t yaml_mapping_colon(const char *content, size_t start,
                                 size_t end) {
  char quote = '\0';
  size_t position;
  for (position = start; position < end; ++position) {
    char byte = content[position];
    if (quote == '"') {
      if (byte == '\\' && position + 1 < end)
        ++position;
      else if (byte == '"')
        quote = '\0';
    } else if (quote == '\'') {
      if (byte == '\'' && position + 1 < end && content[position + 1] == '\'')
        ++position;
      else if (byte == '\'')
        quote = '\0';
    } else if (byte == '"' || byte == '\'') {
      quote = byte;
    } else if (byte == ':' &&
               (position + 1 == end || content[position + 1] == ' ' ||
                content[position + 1] == '\t')) {
      return position;
    }
  }
  return SIZE_MAX;
}

static bool yaml_bare_cr_has_stringifier_error_context(const char *content) {
  size_t length = strlen(content);
  size_t position;
  for (position = 0; position < length; ++position) {
    size_t line_start;
    size_t line_end;
    size_t colon;
    size_t value_start;
    size_t comment;
    size_t tail_start;
    size_t tail_nonspace;
    size_t scan;
    int flow_depth = 0;
    bool closes_flow = false;
    if (content[position] != '\r' ||
        (position + 1 < length && content[position + 1] == '\n'))
      continue;
    line_start = position;
    while (line_start > 0 && content[line_start - 1] != '\n')
      --line_start;
    line_end = position;
    while (line_end < length && content[line_end] != '\n')
      ++line_end;
    colon = yaml_mapping_colon(content, line_start, position);
    if (colon == SIZE_MAX)
      continue;
    value_start = colon + 1;
    while (value_start < position &&
           (content[value_start] == ' ' || content[value_start] == '\t'))
      ++value_start;
    if (value_start >= position || content[value_start] == '\'' ||
        content[value_start] == '"' || content[value_start] == '>' ||
        content[value_start] == '|')
      continue;
    comment = position;
    for (scan = value_start; scan < position; ++scan)
      if (content[scan] == '#' && scan > value_start &&
          (content[scan - 1] == ' ' || content[scan - 1] == '\t')) {
        comment = scan;
        break;
      }
    if (comment < position)
      continue;
    tail_start = position + 1;
    tail_nonspace = tail_start;
    while (tail_nonspace < line_end &&
           (content[tail_nonspace] == ' ' || content[tail_nonspace] == '\t'))
      ++tail_nonspace;
    if (tail_nonspace == line_end)
      continue;
    if (content[tail_nonspace] == '#')
      return true;
    if (position > value_start && content[position - 1] == ':')
      return true;
    if ((content[value_start] == '-' || content[value_start] == '?') &&
        value_start + 1 < position &&
        (content[value_start + 1] == ' ' || content[value_start + 1] == '\t'))
      return true;
    for (scan = value_start; scan < position; ++scan) {
      if (content[scan] == '[' || content[scan] == '{')
        ++flow_depth;
      else if ((content[scan] == ']' || content[scan] == '}') &&
               flow_depth > 0) {
        --flow_depth;
        if (scan + 1 == position)
          closes_flow = true;
      }
    }
    if (closes_flow)
      return true;
  }
  return false;
}

static bool yaml_segment_starts_mapping_key(const char *content, size_t start,
                                            size_t end) {
  size_t comment = end;
  size_t colon;
  while (start < end && (content[start] == ' ' || content[start] == '\t'))
    ++start;
  if (start == end || content[start] == '#')
    return false;
  for (comment = start; comment < end; ++comment)
    if (content[comment] == '#' && comment > start &&
        (content[comment - 1] == ' ' || content[comment - 1] == '\t'))
      break;
  colon = yaml_mapping_colon(content, start, comment);
  return colon != SIZE_MAX;
}

static bool yaml_append_escaped_cr_plain_value(CsemverBuffer *output,
                                               const char *content,
                                               size_t start, size_t end) {
  size_t position;
  if (!csemver_buffer_append(output, "\"", 1))
    return false;
  for (position = start; position < end; ++position) {
    char byte = content[position];
    if (byte == '\r') {
      if (!csemver_buffer_append(output, "\\r", 2))
        return false;
    } else if (byte == '\\' || byte == '"') {
      if (!csemver_buffer_append(output, "\\", 1) ||
          !csemver_buffer_append(output, &byte, 1))
        return false;
    } else if (byte == '\t') {
      if (!csemver_buffer_append(output, "\\t", 2))
        return false;
    } else if (!csemver_buffer_append(output, &byte, 1)) {
      return false;
    }
  }
  return csemver_buffer_append(output, "\"", 1);
}

static int yaml_normalize_single_line_flow(const char *content, char **output,
                                           size_t *output_size);

static int yaml_rewrite_flat_flow_sequence_with_bare_cr(const char *content,
                                                        char **updated,
                                                        size_t *updated_size) {
  size_t length = strlen(content);
  size_t position;
  *updated = NULL;
  *updated_size = 0;
  for (position = 0; position < length; ++position) {
    size_t line_start;
    size_t line_end;
    size_t colon;
    size_t open;
    size_t close = SIZE_MAX;
    size_t scan;
    size_t item_start;
    size_t item_index = 0;
    bool valid = true;
    if (content[position] != '\r' ||
        (position + 1 < length && content[position + 1] == '\n'))
      continue;
    line_start = position;
    while (line_start > 0 && content[line_start - 1] != '\n')
      --line_start;
    line_end = position;
    while (line_end < length && content[line_end] != '\n')
      ++line_end;
    colon = yaml_mapping_colon(content, line_start, position);
    if (colon == SIZE_MAX)
      continue;
    open = colon + 1;
    while (open < position && (content[open] == ' ' || content[open] == '\t'))
      ++open;
    if (open >= position || content[open] != '[')
      continue;
    for (scan = open + 1; scan < line_end; ++scan) {
      char byte = content[scan];
      if (byte == ']') {
        close = scan;
        break;
      }
      if (byte == '[' || byte == '{' || byte == '}' || byte == '#' ||
          byte == '"' || byte == '\'') {
        valid = false;
        break;
      }
      if (byte == ':' && scan + 1 < line_end &&
          (content[scan + 1] == ' ' || content[scan + 1] == '\t' ||
           content[scan + 1] == '\r' || content[scan + 1] == '\n')) {
        valid = false;
        break;
      }
    }
    if (!valid || close == SIZE_MAX || position >= close)
      continue;
    item_start = open + 1;
    for (scan = item_start; scan <= close; ++scan) {
      size_t item_end;
      size_t trimmed_start;
      size_t trimmed_end;
      if (scan != close && content[scan] != ',')
        continue;
      item_end = scan;
      trimmed_start = item_start;
      trimmed_end = item_end;
      while (trimmed_start < trimmed_end &&
             (content[trimmed_start] == ' ' || content[trimmed_start] == '\t'))
        ++trimmed_start;
      while (trimmed_end > trimmed_start && (content[trimmed_end - 1] == ' ' ||
                                             content[trimmed_end - 1] == '\t'))
        --trimmed_end;
      if (trimmed_start == trimmed_end) {
        valid = false;
        break;
      }
      ++item_index;
      item_start = scan + 1;
    }
    if (!valid || item_index == 0)
      continue;
    {
      CsemverBuffer rewritten;
      csemver_buffer_init(&rewritten);
      if (!csemver_buffer_append(&rewritten, content, open + 1) ||
          !csemver_buffer_append(&rewritten, " ", 1)) {
        csemver_buffer_free(&rewritten);
        return 0;
      }
      item_start = open + 1;
      item_index = 0;
      for (scan = item_start; scan <= close; ++scan) {
        size_t item_end;
        size_t trimmed_start;
        size_t trimmed_end;
        if (scan != close && content[scan] != ',')
          continue;
        item_end = scan;
        trimmed_start = item_start;
        trimmed_end = item_end;
        while (trimmed_start < trimmed_end && (content[trimmed_start] == ' ' ||
                                               content[trimmed_start] == '\t'))
          ++trimmed_start;
        while (trimmed_end > trimmed_start &&
               (content[trimmed_end - 1] == ' ' ||
                content[trimmed_end - 1] == '\t'))
          --trimmed_end;
        if (item_index != 0 && !csemver_buffer_append(&rewritten, ", ", 2)) {
          csemver_buffer_free(&rewritten);
          return 0;
        }
        if (memchr(content + trimmed_start, '\r',
                   trimmed_end - trimmed_start) != NULL) {
          if (!yaml_append_escaped_cr_plain_value(&rewritten, content,
                                                  trimmed_start, trimmed_end)) {
            csemver_buffer_free(&rewritten);
            return 0;
          }
        } else if (!csemver_buffer_append(&rewritten, content + trimmed_start,
                                          trimmed_end - trimmed_start)) {
          csemver_buffer_free(&rewritten);
          return 0;
        }
        ++item_index;
        item_start = scan + 1;
      }
      if (!csemver_buffer_append(&rewritten, " ]", 2) ||
          !csemver_buffer_append(&rewritten, content + close + 1,
                                 length - close - 1)) {
        csemver_buffer_free(&rewritten);
        return 0;
      }
      *updated = rewritten.data;
      *updated_size = rewritten.length;
      return 1;
    }
  }
  return 1;
}

static int yaml_rewrite_flow_plain_value_with_bare_cr_once(
    const char *content, char **updated, size_t *updated_size,
    bool *requires_normalization) {
  *requires_normalization = false;
  if (!yaml_rewrite_flat_flow_sequence_with_bare_cr(content, updated,
                                                    updated_size))
    return 0;
  if (*updated != NULL)
    return 1;
  *requires_normalization = true;
  size_t length = strlen(content);
  size_t position;
  *updated = NULL;
  *updated_size = 0;
  for (position = 0; position < length; ++position) {
    size_t line_start;
    size_t colon;
    size_t value_start;
    size_t scalar_start;
    size_t scalar_end;
    size_t collection_start;
    size_t collection_end;
    size_t flow_starts[128];
    size_t scan;
    int flow_depth = 0;
    int candidate_depth;
    char quote = '\0';
    bool escaped = false;
    bool found_boundary = false;
    bool found_collection_end = false;
    bool flow_comment = false;
    bool flow_stack_overflow = false;
    if (content[position] != '\r' ||
        (position + 1 < length && content[position + 1] == '\n'))
      continue;
    line_start = position;
    while (line_start > 0 && content[line_start - 1] != '\n')
      --line_start;
    colon = yaml_mapping_colon(content, line_start, position);
    if (colon == SIZE_MAX)
      continue;
    value_start = colon + 1;
    while (value_start < position &&
           (content[value_start] == ' ' || content[value_start] == '\t'))
      ++value_start;
    scalar_start = value_start;
    for (scan = value_start; scan < position; ++scan) {
      char byte = content[scan];
      if (quote == '"') {
        if (escaped)
          escaped = false;
        else if (byte == '\\')
          escaped = true;
        else if (byte == '"')
          quote = '\0';
      } else if (quote == '\'') {
        if (byte == '\'')
          quote = '\0';
      } else if (byte == '"' || byte == '\'') {
        quote = byte;
      } else if (byte == '#' && scan > value_start &&
                 (content[scan - 1] == ' ' || content[scan - 1] == '\t')) {
        flow_comment = true;
        break;
      } else if (byte == '[' || byte == '{') {
        if (flow_depth >= (int)(sizeof flow_starts / sizeof flow_starts[0])) {
          flow_stack_overflow = true;
          break;
        }
        flow_starts[flow_depth] = scan;
        ++flow_depth;
        scalar_start = scan + 1;
      } else if ((byte == ']' || byte == '}') && flow_depth > 0) {
        --flow_depth;
        scalar_start = scan + 1;
      } else if (byte == ',' && flow_depth > 0) {
        scalar_start = scan + 1;
      } else if (byte == ':' && flow_depth > 0 &&
                 ((scan + 1 < position &&
                   (content[scan + 1] == ' ' || content[scan + 1] == '\t')) ||
                  (scan + 1 == position && content[position] == '\r'))) {
        scalar_start = scan + 1;
      }
    }
    if (flow_comment || flow_stack_overflow || flow_depth == 0 || quote != '\0')
      continue;
    candidate_depth = flow_depth;
    collection_start = flow_starts[flow_depth - 1];
    while (scalar_start < position &&
           (content[scalar_start] == ' ' || content[scalar_start] == '\t'))
      ++scalar_start;
    if (scalar_start > position)
      continue;
    scalar_end = length;
    flow_depth = candidate_depth;
    quote = '\0';
    escaped = false;
    for (scan = position + 1; scan < length; ++scan) {
      char byte = content[scan];
      if (byte == '\n' && quote == '\0')
        break;
      if (quote == '"') {
        if (escaped)
          escaped = false;
        else if (byte == '\\')
          escaped = true;
        else if (byte == '"')
          quote = '\0';
      } else if (quote == '\'') {
        if (byte == '\'')
          quote = '\0';
      } else if (byte == '"' || byte == '\'') {
        quote = byte;
      } else if (byte == '[' || byte == '{') {
        ++flow_depth;
      } else if ((byte == ']' || byte == '}') && flow_depth > 0) {
        if (flow_depth == candidate_depth) {
          scalar_end = scan;
          found_boundary = true;
          break;
        }
        --flow_depth;
      } else if (byte == ',' && flow_depth == candidate_depth) {
        scalar_end = scan;
        found_boundary = true;
        break;
      }
    }
    if (!found_boundary)
      continue;
    while (scalar_end > scalar_start &&
           (content[scalar_end - 1] == ' ' || content[scalar_end - 1] == '\t'))
      --scalar_end;
    flow_depth = candidate_depth;
    quote = '\0';
    escaped = false;
    for (scan = position + 1; scan < length; ++scan) {
      char byte = content[scan];
      if (byte == '\n' && quote == '\0')
        break;
      if (quote == '"') {
        if (escaped)
          escaped = false;
        else if (byte == '\\')
          escaped = true;
        else if (byte == '"')
          quote = '\0';
      } else if (quote == '\'') {
        if (byte == '\'')
          quote = '\0';
      } else if (byte == '"' || byte == '\'') {
        quote = byte;
      } else if (byte == '[' || byte == '{') {
        ++flow_depth;
      } else if ((byte == ']' || byte == '}') && flow_depth > 0) {
        if (flow_depth == candidate_depth) {
          collection_end = scan;
          found_collection_end = true;
          break;
        }
        --flow_depth;
      }
    }
    if (!found_collection_end || scalar_end <= position)
      continue;
    {
      CsemverBuffer rewritten;
      csemver_buffer_init(&rewritten);
      if (!csemver_buffer_append(&rewritten, content, collection_start + 1) ||
          (collection_start + 1 < length &&
           content[collection_start + 1] != ' ' &&
           content[collection_start + 1] != '\t' &&
           !csemver_buffer_append(&rewritten, " ", 1)) ||
          !csemver_buffer_append(&rewritten, content + collection_start + 1,
                                 scalar_start - collection_start - 1) ||
          (scalar_start > collection_start + 1 &&
           (content[scalar_start - 1] == ',' ||
            content[scalar_start - 1] == ':') &&
           !csemver_buffer_append(&rewritten, " ", 1)) ||
          !yaml_append_escaped_cr_plain_value(&rewritten, content, scalar_start,
                                              scalar_end) ||
          !csemver_buffer_append(&rewritten, content + scalar_end,
                                 collection_end - scalar_end) ||
          (collection_end > 0 && content[collection_end - 1] != ' ' &&
           content[collection_end - 1] != '\t' &&
           content[collection_end - 1] != '\n' &&
           content[collection_end - 1] != '\r' &&
           !csemver_buffer_append(&rewritten, " ", 1)) ||
          !csemver_buffer_append(&rewritten, content + collection_end,
                                 length - collection_end)) {
        csemver_buffer_free(&rewritten);
        return 0;
      }
      *updated = rewritten.data;
      *updated_size = rewritten.length;
      return 1;
    }
  }
  return 1;
}

static int yaml_rewrite_flow_plain_value_with_bare_cr(const char *content,
                                                      char **updated,
                                                      size_t *updated_size) {
  size_t length = strlen(content);
  size_t line_start = 0;
  size_t position;
  bool changed = false;
  CsemverBuffer output;
  *updated = NULL;
  *updated_size = 0;
  for (position = 0; position < length; ++position)
    if (content[position] == '\r' &&
        (position + 1 == length || content[position + 1] != '\n'))
      break;
  if (position == length)
    return 1;
  csemver_buffer_init(&output);
  while (line_start < length) {
    size_t line_end = line_start;
    size_t line_size;
    bool line_needs_normalization = false;
    char *line;
    while (line_end < length && content[line_end] != '\n')
      ++line_end;
    if (line_end < length)
      ++line_end;
    line_size = line_end - line_start;
    line = malloc(line_size + 1);
    if (line == NULL) {
      csemver_buffer_free(&output);
      return 0;
    }
    memcpy(line, content + line_start, line_size);
    line[line_size] = '\0';
    for (;;) {
      char *rewritten_line = NULL;
      size_t rewritten_line_size = 0;
      bool requires_normalization = false;
      if (!yaml_rewrite_flow_plain_value_with_bare_cr_once(
              line, &rewritten_line, &rewritten_line_size,
              &requires_normalization)) {
        free(line);
        csemver_buffer_free(&output);
        return 0;
      }
      if (rewritten_line == NULL)
        break;
      if (requires_normalization)
        line_needs_normalization = true;
      free(line);
      line = rewritten_line;
      line_size = rewritten_line_size;
      changed = true;
    }
    if (line_needs_normalization) {
      char *normalized_line = NULL;
      size_t normalized_line_size = 0;
      size_t byte;
      bool has_bare_cr = false;
      for (byte = 0; byte < line_size; ++byte)
        if (line[byte] == '\r' &&
            (byte + 1 == line_size || line[byte + 1] != '\n')) {
          has_bare_cr = true;
          break;
        }
      if (!has_bare_cr) {
        if (!yaml_normalize_single_line_flow(line, &normalized_line,
                                             &normalized_line_size)) {
          free(line);
          csemver_buffer_free(&output);
          return 0;
        }
        if (normalized_line_size >= 9 &&
            memcmp(normalized_line + normalized_line_size - 9, "undefined",
                   9) == 0) {
          normalized_line_size -= 9;
          normalized_line[normalized_line_size] = '\0';
        }
        free(line);
        line = normalized_line;
        line_size = normalized_line_size;
      }
    }
    if (!csemver_buffer_append(&output, line, line_size)) {
      free(line);
      csemver_buffer_free(&output);
      return 0;
    }
    free(line);
    line_start = line_end;
  }
  if (!changed) {
    csemver_buffer_free(&output);
    *updated = NULL;
    *updated_size = 0;
    return 1;
  }
  *updated = output.data;
  *updated_size = output.length;
  return 1;
}

static int yaml_sanitize_bare_cr_comment(const char *content, char **sanitized,
                                         bool *changed) {
  size_t length = strlen(content);
  size_t position;
  size_t line_start = 0;
  bool in_comment = false;
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escaped = false;
  bool other_bare_cr = false;
  CsemverBuffer output;
  *sanitized = NULL;
  *changed = false;
  if (memchr(content, '\r', length) == NULL)
    return 1;
  csemver_buffer_init(&output);
  for (position = 0; position < length; ++position) {
    char byte = content[position];
    if (byte == '\r' &&
        (position + 1 == length || content[position + 1] != '\n')) {
      if (in_comment) {
        if (!csemver_buffer_append(&output, " ", 1)) {
          csemver_buffer_free(&output);
          return 0;
        }
        *changed = true;
      } else {
        if (!csemver_buffer_append(&output, &byte, 1)) {
          csemver_buffer_free(&output);
          return 0;
        }
        other_bare_cr = true;
      }
      continue;
    }
    if (!in_comment) {
      if (in_double_quote) {
        if (escaped)
          escaped = false;
        else if (byte == '\\')
          escaped = true;
        else if (byte == '"')
          in_double_quote = false;
      } else if (in_single_quote) {
        if (byte == '\'')
          in_single_quote = false;
      } else if (byte == '"') {
        in_double_quote = true;
      } else if (byte == '\'') {
        in_single_quote = true;
      } else if (byte == '#' &&
                 (position == line_start || content[position - 1] == ' ' ||
                  content[position - 1] == '\t')) {
        in_comment = true;
      }
    }
    if (!csemver_buffer_append(&output, &byte, 1)) {
      csemver_buffer_free(&output);
      return 0;
    }
    if (byte == '\n') {
      line_start = position + 1;
      in_comment = false;
      in_single_quote = false;
      in_double_quote = false;
      escaped = false;
    }
  }
  if (!*changed || other_bare_cr) {
    csemver_buffer_free(&output);
    *changed = false;
    return 1;
  }
  *sanitized = output.data;
  return 1;
}

static int yaml_expand_bare_cr_comment_markers(const char *content,
                                               size_t source_offset,
                                               char **expanded,
                                               size_t *expanded_size,
                                               size_t *expanded_offset) {
  size_t length = strlen(content);
  size_t position;
  size_t line_start = 0;
  size_t inserted_before_offset = 0;
  size_t removed_before_offset = 0;
  size_t comment_indent = 0;
  size_t comment_body_start = 0;
  size_t comment_marker_output_position = 0;
  bool comment_is_inline = false;
  bool in_comment = false;
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escaped = false;
  CsemverBuffer output;
  *expanded = NULL;
  *expanded_size = 0;
  *expanded_offset = source_offset;
  csemver_buffer_init(&output);
  for (position = 0; position < length; ++position) {
    char byte = content[position];
    if (byte == '\r' &&
        (position + 1 == length || content[position + 1] != '\n') &&
        in_comment) {
      size_t body_position;
      bool empty_comment_body = true;
      size_t next = position + 1;
      bool nonempty_comment_line = false;
      for (body_position = comment_body_start; body_position < position;
           ++body_position)
        if (content[body_position] != ' ' && content[body_position] != '\t')
          empty_comment_body = false;
      if (empty_comment_body) {
        if (comment_is_inline) {
          size_t next = position + 1;
          bool nonempty_comment_line = false;
          while (next < length && content[next] != '\r' &&
                 content[next] != '\n') {
            if (content[next] != ' ' && content[next] != '\t')
              nonempty_comment_line = true;
            ++next;
          }
          if (comment_marker_output_position < output.length &&
              output.data[comment_marker_output_position] == '#') {
            memmove(output.data + comment_marker_output_position,
                    output.data + comment_marker_output_position + 1,
                    output.length - comment_marker_output_position);
            --output.length;
          }
          if (!csemver_buffer_append(&output, &byte, 1) ||
              (nonempty_comment_line &&
               !csemver_buffer_append(&output, "#", 1))) {
            csemver_buffer_free(&output);
            return 0;
          }
          if (position < source_offset) {
            ++removed_before_offset;
            if (nonempty_comment_line)
              ++inserted_before_offset;
          }
        } else if (position < source_offset) {
          ++removed_before_offset;
        }
        continue;
      }
      while (next < length && content[next] != '\r' && content[next] != '\n') {
        if (content[next] != ' ' && content[next] != '\t')
          nonempty_comment_line = true;
        ++next;
      }
      if (!csemver_buffer_append(&output, &byte, 1) ||
          (nonempty_comment_line &&
           (!csemver_buffer_append(&output, content + line_start,
                                   comment_indent) ||
            !csemver_buffer_append(&output, "#", 1)))) {
        csemver_buffer_free(&output);
        return 0;
      }
      if (nonempty_comment_line && position < source_offset)
        inserted_before_offset += comment_indent + 1;
      continue;
    }
    if (!in_comment) {
      if (in_double_quote) {
        if (escaped)
          escaped = false;
        else if (byte == '\\')
          escaped = true;
        else if (byte == '"')
          in_double_quote = false;
      } else if (in_single_quote) {
        if (byte == '\'')
          in_single_quote = false;
      } else if (byte == '"') {
        in_double_quote = true;
      } else if (byte == '\'') {
        in_single_quote = true;
      } else if (byte == '#' &&
                 (position == line_start || content[position - 1] == ' ' ||
                  content[position - 1] == '\t')) {
        size_t indent_start = position;
        while (indent_start > line_start && (content[indent_start - 1] == ' ' ||
                                             content[indent_start - 1] == '\t'))
          --indent_start;
        comment_indent = indent_start == line_start ? position - line_start : 0;
        comment_body_start = position + 1;
        comment_is_inline = indent_start != line_start;
        comment_marker_output_position = output.length;
        in_comment = true;
      }
    }
    if (!csemver_buffer_append(&output, &byte, 1)) {
      csemver_buffer_free(&output);
      return 0;
    }
    if (byte == '\n') {
      line_start = position + 1;
      comment_indent = 0;
      comment_body_start = 0;
      comment_is_inline = false;
      in_comment = false;
      in_single_quote = false;
      in_double_quote = false;
      escaped = false;
    }
  }
  *expanded = output.data;
  *expanded_size = output.length;
  *expanded_offset =
      source_offset + inserted_before_offset - removed_before_offset;
  return 1;
}

static int yaml_rewrite_plain_values_with_bare_cr(const char *content,
                                                  char **updated,
                                                  size_t *updated_size) {
  size_t length = strlen(content);
  size_t copy_position = 0;
  size_t position = 0;
  bool changed = false;
  CsemverBuffer rewritten;
  *updated = NULL;
  *updated_size = 0;
  if (memchr(content, '\r', length) == NULL)
    return 1;
  csemver_buffer_init(&rewritten);
  while (position < length) {
    if (content[position] == '\r' &&
        (position + 1 == length || content[position + 1] != '\n')) {
      size_t line_start = position;
      size_t line_end = position;
      size_t colon;
      size_t value_start;
      size_t value_end;
      size_t comment_start;
      size_t tail_start = position + 1;
      size_t tail_nonspace;
      while (line_start > 0 && content[line_start - 1] != '\n')
        --line_start;
      while (line_end < length && content[line_end] != '\n')
        ++line_end;
      colon = yaml_mapping_colon(content, line_start, position);
      if (colon != SIZE_MAX) {
        value_start = colon + 1;
        while (value_start < position &&
               (content[value_start] == ' ' || content[value_start] == '\t'))
          ++value_start;
        if (value_start <= position && content[value_start] != '#' &&
            content[value_start] != '\'' && content[value_start] != '"' &&
            content[value_start] != '>' && content[value_start] != '|' &&
            content[value_start] != '&' && content[value_start] != '!' &&
            content[value_start] != '[' && content[value_start] != '{' &&
            !((content[value_start] == '-' || content[value_start] == '?') &&
              value_start + 1 < position &&
              (content[value_start + 1] == ' ' ||
               content[value_start + 1] == '\t')) &&
            !(position > value_start && content[position - 1] == ':')) {
          tail_nonspace = tail_start;
          while (tail_nonspace < line_end && (content[tail_nonspace] == ' ' ||
                                              content[tail_nonspace] == '\t'))
            ++tail_nonspace;
          if (tail_nonspace < line_end && content[tail_nonspace] != '#' &&
              !yaml_segment_starts_mapping_key(content, tail_start, line_end)) {
            comment_start = line_end;
            for (value_end = value_start; value_end < line_end; ++value_end)
              if (content[value_end] == '#' && value_end > value_start &&
                  (content[value_end - 1] == ' ' ||
                   content[value_end - 1] == '\t')) {
                comment_start = value_end;
                break;
              }
            value_end = comment_start;
            while (value_end > value_start && (content[value_end - 1] == ' ' ||
                                               content[value_end - 1] == '\t'))
              --value_end;
            if (value_end > position && position >= value_start) {
              if (!csemver_buffer_append(&rewritten, content + copy_position,
                                         value_start - copy_position) ||
                  !yaml_append_escaped_cr_plain_value(&rewritten, content,
                                                      value_start, value_end)) {
                csemver_buffer_free(&rewritten);
                return 0;
              }
              if (comment_start < line_end &&
                  (!csemver_buffer_append(&rewritten, " ", 1) ||
                   !csemver_buffer_append(&rewritten, content + comment_start,
                                          line_end - comment_start))) {
                csemver_buffer_free(&rewritten);
                return 0;
              }
              copy_position = line_end;
              position = line_end;
              changed = true;
              continue;
            }
          }
        }
      }
    }
    ++position;
  }
  if (!changed) {
    csemver_buffer_free(&rewritten);
    return 1;
  }
  if (!csemver_buffer_append(&rewritten, content + copy_position,
                             length - copy_position)) {
    csemver_buffer_free(&rewritten);
    return 0;
  }
  *updated = rewritten.data;
  *updated_size = rewritten.length;
  return 1;
}

static int yaml_escape_bare_cr_in_double_quoted_scalars(const char *content,
                                                        char **updated,
                                                        size_t *updated_size) {
  size_t length = strlen(content);
  size_t copy_position = 0;
  bool changed = false;
  bool failed = false;
  yaml_parser_t parser;
  CsemverBuffer escaped;
  *updated = NULL;
  *updated_size = 0;
  if (memchr(content, '\r', length) == NULL)
    return 1;
  csemver_buffer_init(&escaped);
  if (!yaml_parser_initialize(&parser))
    return 0;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content, length);
  for (;;) {
    yaml_event_t event;
    bool stream_end;
    if (!yaml_parser_parse(&parser, &event)) {
      failed = true;
      break;
    }
    if (event.type == YAML_SCALAR_EVENT &&
        event.data.scalar.style == YAML_DOUBLE_QUOTED_SCALAR_STYLE) {
      size_t start;
      size_t end;
      size_t position;
      if (!yaml_mark_to_byte_offset(content, event.start_mark.index, &start) ||
          !yaml_mark_to_byte_offset(content, event.end_mark.index, &end) ||
          start < copy_position || end < start || end > length) {
        failed = true;
      } else {
        for (position = start; position < end; ++position) {
          if (content[position] == '\r' &&
              (position + 1 == end || content[position + 1] != '\n')) {
            if (!csemver_buffer_append(&escaped, content + copy_position,
                                       position - copy_position) ||
                !csemver_buffer_append(&escaped, "\\r", 2)) {
              failed = true;
              break;
            }
            copy_position = position + 1;
            changed = true;
          }
        }
      }
    }
    stream_end = event.type == YAML_STREAM_END_EVENT;
    yaml_event_delete(&event);
    if (failed || stream_end)
      break;
  }
  yaml_parser_delete(&parser);
  if (!failed && changed &&
      !csemver_buffer_append(&escaped, content + copy_position,
                             length - copy_position))
    failed = true;
  if (failed) {
    csemver_buffer_free(&escaped);
    return 0;
  }
  if (!changed) {
    csemver_buffer_free(&escaped);
    return 1;
  }
  *updated = escaped.data;
  *updated_size = escaped.length;
  return 1;
}

static int yaml_previous_explicit_version_key(const char *content,
                                              size_t line_start,
                                              size_t key_start,
                                              size_t *explicit_key_start,
                                              size_t *version_key_start,
                                              size_t *version_key_end) {
  size_t previous_end = line_start;
  size_t previous_start;
  size_t previous_indent;
  size_t current_indent_size = key_start - line_start;
  size_t key_end;
  size_t position;
  while (previous_end > 0 && (content[previous_end - 1] == '\n' ||
                              content[previous_end - 1] == '\r'))
    --previous_end;
  previous_start = previous_end;
  while (previous_start > 0 && content[previous_start - 1] != '\n' &&
         content[previous_start - 1] != '\r')
    --previous_start;
  previous_indent = previous_start;
  while (previous_indent < previous_end &&
         (content[previous_indent] == ' ' || content[previous_indent] == '\t'))
    ++previous_indent;
  if (previous_indent - previous_start != current_indent_size ||
      memcmp(content + previous_start, content + line_start,
             current_indent_size) != 0 ||
      previous_indent == previous_end || content[previous_indent] != '?')
    return 0;
  position = previous_indent + 1;
  if (position < previous_end && content[position] != ' ' &&
      content[position] != '\t')
    return 0;
  while (position < previous_end &&
         (content[position] == ' ' || content[position] == '\t'))
    ++position;
  key_end = previous_end;
  while (key_end > position &&
         (content[key_end - 1] == ' ' || content[key_end - 1] == '\t'))
    --key_end;
  if (key_end - position == sizeof "version" - 1 &&
      memcmp(content + position, "version", sizeof "version" - 1) == 0) {
    *version_key_start = position;
    *version_key_end = key_end;
  } else if (key_end - position == sizeof "\"version\"" - 1 &&
             ((content[position] == '"' && content[key_end - 1] == '"') ||
              (content[position] == '\'' && content[key_end - 1] == '\'')) &&
             memcmp(content + position + 1, "version", sizeof "version" - 1) ==
                 0) {
    *version_key_start = position;
    *version_key_end = key_end;
  } else {
    return 0;
  }
  *explicit_key_start = previous_indent;
  return 1;
}

static int yaml_normalize_version_mapping_key(CsemverBuffer *buffer,
                                              bool openapi) {
  Range range;
  yaml_scalar_style_t style;
  size_t line_start;
  size_t key_start;
  size_t key_end;
  size_t value_start;
  size_t explicit_key_start;
  size_t version_key_start;
  size_t version_key_end;
  size_t colon = SIZE_MAX;
  size_t position;
  CsemverBuffer normalized;
  if (!yaml_version_range(buffer->data, openapi, &range, NULL, 0, NULL, &style,
                          NULL) ||
      range.start > buffer->length)
    return 1;
  value_start = range.start;
  if (style == YAML_SINGLE_QUOTED_SCALAR_STYLE ||
      style == YAML_DOUBLE_QUOTED_SCALAR_STYLE) {
    if (value_start == 0 || (buffer->data[value_start - 1] != '\'' &&
                             buffer->data[value_start - 1] != '"'))
      return 1;
    --value_start;
  } else if (style != YAML_PLAIN_SCALAR_STYLE) {
    return 1;
  }
  line_start = value_start;
  while (line_start > 0 && buffer->data[line_start - 1] != '\n' &&
         buffer->data[line_start - 1] != '\r')
    --line_start;
  for (position = line_start; position < value_start; ++position) {
    if (buffer->data[position] == ':')
      colon = position;
  }
  if (colon == SIZE_MAX)
    return 1;
  for (position = colon + 1; position < value_start; ++position) {
    if (buffer->data[position] != ' ' && buffer->data[position] != '\t')
      return 1;
  }
  key_start = line_start;
  while (key_start < colon &&
         (buffer->data[key_start] == ' ' || buffer->data[key_start] == '\t'))
    ++key_start;
  key_end = colon;
  while (key_end > key_start && (buffer->data[key_end - 1] == ' ' ||
                                 buffer->data[key_end - 1] == '\t'))
    --key_end;
  if (key_end == key_start && colon == key_start &&
      yaml_previous_explicit_version_key(
          buffer->data, line_start, key_start, &explicit_key_start,
          &version_key_start, &version_key_end)) {
    csemver_buffer_init(&normalized);
    if (!csemver_buffer_append(&normalized, buffer->data, explicit_key_start) ||
        !csemver_buffer_append(&normalized, buffer->data + version_key_start,
                               version_key_end - version_key_start) ||
        !csemver_buffer_append(&normalized, ": ", 2) ||
        !csemver_buffer_append(&normalized, buffer->data + value_start,
                               buffer->length - value_start)) {
      csemver_buffer_free(&normalized);
      return 0;
    }
    csemver_buffer_free(buffer);
    *buffer = normalized;
    return 1;
  }
  if (key_end - key_start != sizeof "version" - 1 ||
      memcmp(buffer->data + key_start, "version", sizeof "version" - 1) != 0 ||
      (colon == key_end && value_start == colon + 2 &&
       buffer->data[colon + 1] == ' '))
    return 1;
  csemver_buffer_init(&normalized);
  if (!csemver_buffer_append(&normalized, buffer->data, key_start) ||
      !csemver_buffer_append(&normalized,
                             "version: ", sizeof "version: " - 1) ||
      !csemver_buffer_append(&normalized, buffer->data + value_start,
                             buffer->length - value_start)) {
    csemver_buffer_free(&normalized);
    return 0;
  }
  csemver_buffer_free(buffer);
  *buffer = normalized;
  return 1;
}

static int yaml_normalize_version_line_spacing(CsemverBuffer *buffer,
                                               bool openapi) {
  size_t scalar_end;
  size_t whitespace_end;
  yaml_scalar_style_t style;
  bool before_comment;
  CsemverBuffer normalized;
  if (!yaml_normalize_version_mapping_key(buffer, openapi))
    return 0;
  if (!yaml_version_range(buffer->data, openapi, NULL, NULL, 0, &scalar_end,
                          &style, NULL) ||
      scalar_end > buffer->length ||
      (style != YAML_PLAIN_SCALAR_STYLE &&
       style != YAML_SINGLE_QUOTED_SCALAR_STYLE &&
       style != YAML_DOUBLE_QUOTED_SCALAR_STYLE))
    return 1;
  whitespace_end = scalar_end;
  while (whitespace_end < buffer->length &&
         (buffer->data[whitespace_end] == ' ' ||
          buffer->data[whitespace_end] == '\t'))
    ++whitespace_end;
  before_comment =
      whitespace_end > scalar_end && buffer->data[whitespace_end] == '#';
  if (!before_comment && buffer->data[whitespace_end] != '\0' &&
      buffer->data[whitespace_end] != '\r' &&
      buffer->data[whitespace_end] != '\n')
    return 1;
  if ((!before_comment && whitespace_end == scalar_end) ||
      (before_comment && whitespace_end == scalar_end + 1 &&
       buffer->data[scalar_end] == ' '))
    return 1;
  csemver_buffer_init(&normalized);
  if (!csemver_buffer_append(&normalized, buffer->data, scalar_end) ||
      (before_comment && !csemver_buffer_append(&normalized, " ", 1)) ||
      !csemver_buffer_append(&normalized, buffer->data + whitespace_end,
                             buffer->length - whitespace_end)) {
    csemver_buffer_free(&normalized);
    return 0;
  }
  csemver_buffer_free(buffer);
  *buffer = normalized;
  return 1;
}

static bool yaml_flow_whitespace(char byte) {
  return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static int yaml_append_flow_comment_block(CsemverBuffer *output,
                                          const char *content, size_t start,
                                          size_t length,
                                          bool first_on_new_line) {
  size_t end = start + length;
  size_t position = start;
  bool first = true;
  while (position < end) {
    size_t line_end = position;
    const char *prefix = first && !first_on_new_line ? " " : "\n  ";
    size_t prefix_length = first && !first_on_new_line ? 1 : 3;
    while (line_end < end && content[line_end] != '\r' &&
           content[line_end] != '\n')
      ++line_end;
    if (!csemver_buffer_append(output, prefix, prefix_length) ||
        !csemver_buffer_append(output, content + position, line_end - position))
      return 0;
    position = line_end;
    while (position < end && yaml_flow_whitespace(content[position]))
      ++position;
    if (position < end && content[position] != '#')
      return 0;
    first = false;
  }
  return 1;
}

static int yaml_append_normalized_flow_fragment(const char *content,
                                                size_t start, size_t end,
                                                CsemverBuffer *output) {
  char *fragment;
  char *normalized = NULL;
  size_t normalized_size = 0;
  size_t length;
  int success = 0;
  if (end < start)
    return 0;
  while (start < end && yaml_flow_whitespace(content[start]))
    ++start;
  while (end > start && yaml_flow_whitespace(content[end - 1]))
    --end;
  length = end - start;
  if (length == 0 || memchr(content + start, '\n', length) != NULL ||
      memchr(content + start, '\r', length) != NULL ||
      memchr(content + start, '#', length) != NULL)
    return 0;
  fragment = malloc(length + 1);
  if (fragment == NULL)
    return 0;
  memcpy(fragment, content + start, length);
  fragment[length] = '\0';
  if (yaml_normalize_single_line_flow(fragment, &normalized,
                                      &normalized_size) &&
      normalized_size >= 9 &&
      memcmp(normalized + normalized_size - 9, "undefined", 9) == 0) {
    normalized_size -= 9;
    success = csemver_buffer_append(output, normalized, normalized_size);
  }
  free(normalized);
  free(fragment);
  return success;
}

static int yaml_flow_sequence_has_leading_comment(const char *content) {
  size_t flow_depth = 0;
  size_t sequence_depth = SIZE_MAX;
  size_t sequence_open_end = 0;
  size_t index;
  bool sequence_pending = false;
  bool failed = false;
  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser))
    return -1;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content,
                               strlen(content));
  for (;;) {
    yaml_token_t token;
    yaml_token_type_t type;
    bool done;
    size_t start;
    size_t end;
    if (!yaml_parser_scan(&parser, &token)) {
      failed = true;
      break;
    }
    type = token.type;
    done = type == YAML_STREAM_END_TOKEN;
    if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
        !yaml_mark_to_byte_offset(content, token.end_mark.index, &end)) {
      failed = true;
    } else {
      if (sequence_pending && flow_depth == sequence_depth) {
        for (index = sequence_open_end; index < start; ++index) {
          if (content[index] == '#') {
            yaml_token_delete(&token);
            yaml_parser_delete(&parser);
            return 1;
          }
        }
        sequence_pending = false;
      }
      if (type == YAML_FLOW_MAPPING_START_TOKEN ||
          type == YAML_FLOW_SEQUENCE_START_TOKEN) {
        ++flow_depth;
        if (type == YAML_FLOW_SEQUENCE_START_TOKEN) {
          sequence_pending = true;
          sequence_depth = flow_depth;
          sequence_open_end = end;
        }
      } else if (type == YAML_FLOW_MAPPING_END_TOKEN ||
                 type == YAML_FLOW_SEQUENCE_END_TOKEN) {
        if (flow_depth > 0)
          --flow_depth;
      }
    }
    yaml_token_delete(&token);
    if (failed || done)
      break;
  }
  yaml_parser_delete(&parser);
  return failed ? -1 : 0;
}

static size_t yaml_line_start(const char *content, size_t position) {
  while (position > 0 && content[position - 1] != '\n' &&
         content[position - 1] != '\r')
    --position;
  return position;
}

static size_t yaml_line_end(const char *content, size_t length,
                            size_t position) {
  while (position < length && content[position] != '\n' &&
         content[position] != '\r')
    ++position;
  return position;
}

static int yaml_append_block_scalar_header(
    CsemverBuffer *output, const char *content, size_t header_start,
    size_t header_end, size_t style_position, const yaml_token_t *token) {
  size_t modifiers_end = style_position + 1;
  size_t trailing_newlines = 0;
  size_t explicit_indent = 0;
  size_t body_line_start;
  size_t first_body_indent = 0;
  size_t content_length = strlen(content);
  size_t header_line_start;
  size_t header_line_indent = 0;
  char desired_chomp = '\0';
  size_t index;
  bool retain_explicit_indent = false;
  while (modifiers_end < header_end && content[modifiers_end] != ' ' &&
         content[modifiers_end] != '\t' && content[modifiers_end] != '#') {
    if (content[modifiers_end] >= '1' && content[modifiers_end] <= '9') {
      explicit_indent = (size_t)(content[modifiers_end] - '0');
    }
    ++modifiers_end;
  }
  for (index = token->data.scalar.length;
       index > 0 && token->data.scalar.value[index - 1] == '\n'; --index)
    ++trailing_newlines;
  if (token->data.scalar.length != 0) {
    if (trailing_newlines == 0)
      desired_chomp = '-';
    else if (trailing_newlines > 1)
      desired_chomp = '+';
  }
  if (explicit_indent != 0) {
    body_line_start = header_end;
    while (
        body_line_start < content_length &&
        (content[body_line_start] == '\r' || content[body_line_start] == '\n'))
      ++body_line_start;
    while (body_line_start < content_length) {
      size_t line_end = yaml_line_end(content, content_length, body_line_start);
      first_body_indent = 0;
      while (body_line_start + first_body_indent < line_end &&
             (content[body_line_start + first_body_indent] == ' ' ||
              content[body_line_start + first_body_indent] == '\t'))
        ++first_body_indent;
      if (body_line_start + first_body_indent < line_end)
        break;
      body_line_start = line_end;
      while (body_line_start < content_length &&
             (content[body_line_start] == '\r' ||
              content[body_line_start] == '\n'))
        ++body_line_start;
    }
    header_line_start = yaml_line_start(content, style_position);
    while (header_line_start + header_line_indent < style_position &&
           (content[header_line_start + header_line_indent] == ' ' ||
            content[header_line_start + header_line_indent] == '\t'))
      ++header_line_indent;
    retain_explicit_indent =
        first_body_indent > header_line_indent + explicit_indent;
  }
  if (!csemver_buffer_append(output, content + header_start,
                             style_position + 1 - header_start) ||
      (retain_explicit_indent && !csemver_buffer_append(output, "2", 1)) ||
      (desired_chomp != '\0' &&
       !csemver_buffer_append(output, &desired_chomp, 1)) ||
      !csemver_buffer_append(output, content + modifiers_end,
                             header_end - modifiers_end))
    return 0;
  return 1;
}

static size_t yaml_block_scalar_indent_indicator(const char *content,
                                                 size_t style_position,
                                                 size_t header_end) {
  size_t index = style_position + 1;
  while (index < header_end && content[index] != ' ' &&
         content[index] != '\t' && content[index] != '#') {
    if (content[index] >= '1' && content[index] <= '9')
      return (size_t)(content[index] - '0');
    ++index;
  }
  return 0;
}

static int yaml_append_spaces(CsemverBuffer *output, size_t count) {
  static const char spaces[] =
      "                                                                ";
  while (count > 0) {
    size_t chunk = count < sizeof spaces - 1 ? count : sizeof spaces - 1;
    if (!csemver_buffer_append(output, spaces, chunk))
      return 0;
    count -= chunk;
  }
  return 1;
}

static int yaml_append_folded_block_scalar(CsemverBuffer *output,
                                           const char *content,
                                           size_t header_end, size_t end,
                                           const yaml_token_t *token,
                                           size_t output_indentation,
                                           size_t explicit_indent) {
  size_t newline_end = header_end;
  size_t newline_size = 1;
  const char *newline = "\n";
  size_t body_position;
  size_t source_indentation = 0;
  size_t header_line_start;
  size_t header_line_indentation = 0;
  size_t trailing_newlines = 0;
  size_t index;
  bool have_content = false;
  size_t pending_blank_lines = 0;
  bool previous_more_indented = false;
  while (newline_end < end && content[newline_end] != '\n' &&
         content[newline_end] != '\r')
    ++newline_end;
  if (newline_end < end) {
    if (content[newline_end] == '\r' && newline_end + 1 < end &&
        content[newline_end + 1] == '\n')
      newline_size = 2;
    newline = content + newline_end;
  }
  if (!csemver_buffer_append(output, newline, newline_size))
    return 0;
  body_position = newline_end + newline_size;
  for (index = body_position; index < end;) {
    size_t line_end = yaml_line_end(content, end, index);
    size_t leading = 0;
    while (index + leading < line_end && (content[index + leading] == ' ' ||
                                          content[index + leading] == '\t'))
      ++leading;
    if (index + leading < line_end) {
      source_indentation = leading;
      break;
    }
    index = line_end;
    if (index < end && content[index] == '\r' && index + 1 < end &&
        content[index + 1] == '\n')
      index += 2;
    else if (index < end)
      ++index;
  }
  header_line_start = yaml_line_start(content, header_end);
  while (header_line_start + header_line_indentation < header_end &&
         (content[header_line_start + header_line_indentation] == ' ' ||
          content[header_line_start + header_line_indentation] == '\t'))
    ++header_line_indentation;
  if (explicit_indent != 0) {
    if (header_line_indentation > SIZE_MAX - explicit_indent)
      return 0;
    if (header_line_indentation + explicit_indent <= source_indentation)
      source_indentation = header_line_indentation + explicit_indent;
  }
  if (output_indentation == SIZE_MAX) {
    if (header_line_indentation > SIZE_MAX - 2)
      return 0;
    output_indentation = header_line_indentation + 2;
  }
  for (index = token->data.scalar.length;
       index > 0 && token->data.scalar.value[index - 1] == '\n'; --index)
    ++trailing_newlines;
  index = body_position;
  while (index < end) {
    size_t line_end = yaml_line_end(content, end, index);
    size_t leading = 0;
    size_t content_offset;
    bool more_indented;
    while (index + leading < line_end && (content[index + leading] == ' ' ||
                                          content[index + leading] == '\t'))
      ++leading;
    content_offset =
        leading < source_indentation ? leading : source_indentation;
    if (index + content_offset == line_end) {
      ++pending_blank_lines;
    } else {
      more_indented = leading > source_indentation;
      if (!have_content) {
        size_t count;
        for (count = 0; count < pending_blank_lines; ++count)
          if (!yaml_append_spaces(output, output_indentation) ||
              !csemver_buffer_append(output, newline, newline_size))
            return 0;
        if (!yaml_append_spaces(output, output_indentation))
          return 0;
      } else if (pending_blank_lines > 0 || previous_more_indented ||
                 more_indented) {
        size_t count;
        size_t line_breaks = pending_blank_lines + 1;
        for (count = 0; count < line_breaks; ++count)
          if (!csemver_buffer_append(output, newline, newline_size))
            return 0;
        if (!yaml_append_spaces(output, output_indentation))
          return 0;
      } else if (!csemver_buffer_append(output, " ", 1)) {
        return 0;
      }
      if (!csemver_buffer_append(output, content + index + content_offset,
                                 line_end - index - content_offset))
        return 0;
      have_content = true;
      previous_more_indented = more_indented;
      pending_blank_lines = 0;
    }
    index = line_end;
    if (index < end && content[index] == '\r' && index + 1 < end &&
        content[index + 1] == '\n')
      index += 2;
    else if (index < end)
      ++index;
  }
  if (trailing_newlines == 0)
    trailing_newlines = 1;
  for (index = 0; index < trailing_newlines; ++index)
    if (!csemver_buffer_append(output, newline, newline_size))
      return 0;
  return 1;
}

static int yaml_rewrite_folded_scalars(const char *content, char **updated,
                                       size_t *updated_size) {
  size_t length = strlen(content);
  size_t copy_position = 0;
  bool changed = false;
  bool failed = false;
  yaml_parser_t parser;
  CsemverBuffer rewritten;
  *updated = NULL;
  *updated_size = 0;
  csemver_buffer_init(&rewritten);
  if (!yaml_parser_initialize(&parser))
    return 0;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content, length);
  for (;;) {
    yaml_token_t token;
    yaml_token_type_t type;
    bool done;
    size_t start;
    size_t end;
    if (!yaml_parser_scan(&parser, &token)) {
      failed = true;
      break;
    }
    type = token.type;
    done = type == YAML_STREAM_END_TOKEN;
    if (type == YAML_SCALAR_TOKEN &&
        token.data.scalar.style == YAML_FOLDED_SCALAR_STYLE) {
      size_t header_start;
      size_t header_end;
      if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
          !yaml_mark_to_byte_offset(content, token.end_mark.index, &end) ||
          start < copy_position || end < start || end > length) {
        failed = true;
      } else {
        header_start = yaml_line_start(content, start);
        header_end = yaml_line_end(content, length, start);
        if (start < header_start || header_end < start ||
            !csemver_buffer_append(&rewritten, content + copy_position,
                                   start - copy_position) ||
            !yaml_append_block_scalar_header(&rewritten, content, start,
                                             header_end, start, &token) ||
            !yaml_append_folded_block_scalar(&rewritten, content, header_end,
                                             end, &token, SIZE_MAX,
                                             yaml_block_scalar_indent_indicator(
                                                 content, start, header_end))) {
          failed = true;
        } else {
          copy_position = end;
          changed = true;
        }
      }
    }
    yaml_token_delete(&token);
    if (failed || done)
      break;
  }
  yaml_parser_delete(&parser);
  if (!failed && changed &&
      !csemver_buffer_append(&rewritten, content + copy_position,
                             length - copy_position))
    failed = true;
  if (failed) {
    csemver_buffer_free(&rewritten);
    return 0;
  }
  if (!changed || (rewritten.length == length &&
                   memcmp(rewritten.data, content, length) == 0)) {
    csemver_buffer_free(&rewritten);
    return 1;
  }
  *updated = rewritten.data;
  *updated_size = rewritten.length;
  return 1;
}

static int yaml_append_dedented_block_scalar(CsemverBuffer *output,
                                             const char *content, size_t start,
                                             size_t end, size_t dedent) {
  size_t position = start;
  while (position < end) {
    if (content[position] == '\r' || content[position] == '\n') {
      size_t newline_end = position + 1;
      if (content[position] == '\r' && newline_end < end &&
          content[newline_end] == '\n')
        ++newline_end;
      if (!csemver_buffer_append(output, content + position,
                                 newline_end - position))
        return 0;
      position = newline_end;
    } else {
      size_t line_end = yaml_line_end(content, end, position);
      size_t indentation = 0;
      while (position + indentation < line_end && indentation < dedent &&
             (content[position + indentation] == ' ' ||
              content[position + indentation] == '\t'))
        ++indentation;
      if (!csemver_buffer_append(output, content + position + indentation,
                                 line_end - position - indentation))
        return 0;
      position = line_end;
    }
  }
  return 1;
}

static int yaml_rewrite_block_sequence_comments(const char *content,
                                                char **updated,
                                                size_t *updated_size) {
  size_t length = strlen(content);
  size_t copy_position = 0;
  size_t pending_start = SIZE_MAX;
  size_t pending_end = 0;
  size_t pending_line = 0;
  size_t pending_column = 0;
  size_t pending_output_column = 0;
  size_t block_depth = 0;
  bool changed = false;
  bool failed = false;
  yaml_parser_t parser;
  CsemverBuffer rewritten;
  *updated = NULL;
  *updated_size = 0;
  csemver_buffer_init(&rewritten);
  if (!yaml_parser_initialize(&parser))
    return 0;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content, length);
  for (;;) {
    yaml_token_t token;
    yaml_token_type_t type;
    bool done;
    size_t start;
    size_t end;
    if (!yaml_parser_scan(&parser, &token)) {
      failed = true;
      break;
    }
    type = token.type;
    done = type == YAML_STREAM_END_TOKEN;
    if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
        !yaml_mark_to_byte_offset(content, token.end_mark.index, &end)) {
      failed = true;
    } else {
      if (pending_start != SIZE_MAX) {
        if (start >= copy_position && token.start_mark.line > pending_line &&
            token.start_mark.column > pending_column) {
          size_t first_comment = SIZE_MAX;
          size_t dash_line_start = yaml_line_start(content, pending_start);
          size_t node_line_start = yaml_line_start(content, start);
          size_t node_content_start = node_line_start;
          size_t node_line_end = yaml_line_end(content, length, start);
          size_t node_span_end = node_line_end;
          size_t indent_length = pending_output_column;
          size_t dedent = 0;
          bool is_block_scalar =
              type == YAML_SCALAR_TOKEN &&
              (token.data.scalar.style == YAML_LITERAL_SCALAR_STYLE ||
               token.data.scalar.style == YAML_FOLDED_SCALAR_STYLE);
          size_t explicit_indent = 0;
          size_t comment_position;
          CsemverBuffer comments;
          bool valid = dash_line_start >= copy_position &&
                       node_line_start > dash_line_start &&
                       node_content_start <= start && node_line_end >= start;
          if (is_block_scalar) {
            node_span_end = end;
            explicit_indent = yaml_block_scalar_indent_indicator(content, start,
                                                                 node_line_end);
            if (explicit_indent == 0) {
              size_t body_position = node_line_end;
              size_t target_indent = indent_length + 2;
              while (body_position < node_span_end) {
                size_t line_end;
                size_t body_indent = 0;
                while (body_position < node_span_end &&
                       (content[body_position] == '\r' ||
                        content[body_position] == '\n')) {
                  if (content[body_position] == '\r' &&
                      body_position + 1 < node_span_end &&
                      content[body_position + 1] == '\n')
                    body_position += 2;
                  else
                    ++body_position;
                }
                if (body_position >= node_span_end)
                  break;
                line_end = yaml_line_end(content, node_span_end, body_position);
                while (body_position + body_indent < line_end &&
                       (content[body_position + body_indent] == ' ' ||
                        content[body_position + body_indent] == '\t'))
                  ++body_indent;
                if (body_position + body_indent < line_end) {
                  if (body_indent > target_indent)
                    dedent = body_indent - target_indent;
                  break;
                }
                body_position = line_end;
              }
            }
          }
          for (comment_position = dash_line_start;
               valid && comment_position < pending_start; ++comment_position)
            if (content[comment_position] != ' ' &&
                content[comment_position] != '\t')
              valid = false;
          while (node_content_start < start &&
                 (content[node_content_start] == ' ' ||
                  content[node_content_start] == '\t'))
            ++node_content_start;
          if (node_content_start != start)
            valid = false;
          for (comment_position = pending_end;
               valid && comment_position < start; ++comment_position)
            if (content[comment_position] == '#') {
              first_comment = comment_position;
              break;
            }
          csemver_buffer_init(&comments);
          if (valid && first_comment != SIZE_MAX) {
            size_t line_position = dash_line_start;
            bool have_comment = false;
            while (line_position < node_line_start) {
              size_t line_end = yaml_line_end(content, length, line_position);
              size_t text_start = line_position;
              if (line_position == dash_line_start) {
                text_start = pending_end;
                while (text_start < line_end && (content[text_start] == ' ' ||
                                                 content[text_start] == '\t'))
                  ++text_start;
              } else {
                while (text_start < line_end && (content[text_start] == ' ' ||
                                                 content[text_start] == '\t'))
                  ++text_start;
              }
              if (text_start < line_end && content[text_start] == '#') {
                if (!yaml_append_spaces(&comments, indent_length) ||
                    !csemver_buffer_append(&comments, content + text_start,
                                           line_end - text_start) ||
                    !csemver_buffer_append(&comments, "\n", 1)) {
                  failed = true;
                  break;
                }
                have_comment = true;
              } else if (text_start == line_end && have_comment) {
                if (!csemver_buffer_append(&comments, "\n", 1)) {
                  failed = true;
                  break;
                }
              } else if (text_start != line_end) {
                valid = false;
                break;
              }
              line_position = line_end;
              if (line_position < node_line_start) {
                if (content[line_position] == '\r' &&
                    line_position + 1 < length &&
                    content[line_position + 1] == '\n')
                  line_position += 2;
                else
                  ++line_position;
              }
            }
            if (valid && have_comment && !failed) {
              if (!csemver_buffer_append(&rewritten, content + copy_position,
                                         dash_line_start - copy_position) ||
                  !csemver_buffer_append(&rewritten, comments.data,
                                         comments.length) ||
                  !yaml_append_spaces(&rewritten, indent_length) ||
                  !csemver_buffer_append(&rewritten, "- ", 2) ||
                  !(is_block_scalar
                        ? yaml_append_block_scalar_header(
                              &rewritten, content, node_content_start,
                              node_line_end, start, &token)
                        : csemver_buffer_append(
                              &rewritten, content + node_content_start,
                              node_line_end - node_content_start)) ||
                  (is_block_scalar &&
                   !(token.data.scalar.style == YAML_FOLDED_SCALAR_STYLE
                         ? yaml_append_folded_block_scalar(
                               &rewritten, content, node_line_end,
                               node_span_end, &token, indent_length + 2,
                               explicit_indent)
                         : yaml_append_dedented_block_scalar(
                               &rewritten, content, node_line_end,
                               node_span_end, dedent)))) {
                failed = true;
              } else {
                copy_position = node_span_end;
                changed = true;
              }
            }
          }
          csemver_buffer_free(&comments);
        } else if (start >= copy_position &&
                   ((type == YAML_BLOCK_ENTRY_TOKEN &&
                     token.start_mark.line > pending_line &&
                     token.start_mark.column == pending_column) ||
                    (type == YAML_BLOCK_END_TOKEN &&
                     token.start_mark.line > pending_line &&
                     token.start_mark.column <= pending_column))) {
          size_t dash_line_start = yaml_line_start(content, pending_start);
          size_t line_end = yaml_line_end(content, length, pending_end);
          size_t comment_start;
          for (comment_start = pending_end; comment_start < line_end;
               ++comment_start)
            if (content[comment_start] == '#')
              break;
          if (comment_start < line_end) {
            size_t scan;
            bool valid_comment = true;
            for (scan = pending_end; scan < comment_start; ++scan)
              if (content[scan] != ' ' && content[scan] != '\t')
                valid_comment = false;
            if (valid_comment) {
              if (dash_line_start < copy_position ||
                  !csemver_buffer_append(&rewritten, content + copy_position,
                                         dash_line_start - copy_position) ||
                  !yaml_append_spaces(&rewritten, pending_output_column) ||
                  !csemver_buffer_append(&rewritten, "-  ", 3)) {
                failed = true;
              } else {
                copy_position = comment_start;
                changed = true;
              }
            }
          }
        }
        pending_start = SIZE_MAX;
      }
      if (type == YAML_BLOCK_END_TOKEN && block_depth > 0)
        --block_depth;
      else if (type == YAML_BLOCK_MAPPING_START_TOKEN ||
               type == YAML_BLOCK_SEQUENCE_START_TOKEN)
        ++block_depth;
      if (type == YAML_BLOCK_ENTRY_TOKEN && start >= copy_position) {
        size_t line_start = yaml_line_start(content, start);
        size_t output_column =
            block_depth == 1 ? 2
                             : (block_depth > 1 ? (block_depth - 1) * 2 : 0);
        if (changed && line_start >= copy_position) {
          size_t source_column = start - line_start;
          size_t line_end = yaml_line_end(content, length, start);
          size_t value_start = end;
          while (value_start < line_end &&
                 (content[value_start] == ' ' || content[value_start] == '\t'))
            ++value_start;
          if (source_column != output_column && value_start < line_end &&
              content[value_start] != '#' && content[value_start] != '|' &&
              content[value_start] != '>') {
            if (!csemver_buffer_append(&rewritten, content + copy_position,
                                       line_start - copy_position) ||
                !yaml_append_spaces(&rewritten, output_column)) {
              failed = true;
            } else {
              copy_position = start;
            }
          }
        }
        pending_start = start;
        pending_end = end;
        pending_line = token.start_mark.line;
        pending_column = token.start_mark.column;
        pending_output_column = output_column;
      }
    }
    yaml_token_delete(&token);
    if (failed || done)
      break;
  }
  yaml_parser_delete(&parser);
  if (failed) {
    csemver_buffer_free(&rewritten);
    return 0;
  }
  if (!changed) {
    csemver_buffer_free(&rewritten);
    return 1;
  }
  if (!csemver_buffer_append(&rewritten, content + copy_position,
                             length - copy_position)) {
    csemver_buffer_free(&rewritten);
    return 0;
  }
  *updated = rewritten.data;
  *updated_size = rewritten.length;
  return 1;
}

static int yaml_format_multiline_root_flow(const char *content, char **output,
                                           size_t *output_size, bool *handled);
static int yaml_append_nested_flow_comment_fragment(const char *content,
                                                    size_t start, size_t end,
                                                    size_t indent_spaces,
                                                    CsemverBuffer *output);

static int yaml_append_nested_flow_collection_item(const char *content,
                                                   size_t start, size_t end,
                                                   CsemverBuffer *output) {
  char *fragment;
  char *formatted = NULL;
  size_t formatted_size = 0;
  size_t position = 0;
  size_t line_index = 0;
  bool handled = false;
  while (start < end && yaml_flow_whitespace(content[start]))
    ++start;
  while (end > start && yaml_flow_whitespace(content[end - 1]))
    --end;
  if (start == end)
    return 0;
  if (content[start] == '[') {
    const char prefix[] = "value:\n    ";
    CsemverBuffer wrapped;
    CsemverBuffer formatted_sequence;
    csemver_buffer_init(&wrapped);
    csemver_buffer_init(&formatted_sequence);
    if (!csemver_buffer_append(&wrapped, "value: ", 7) ||
        !csemver_buffer_append(&wrapped, content + start, end - start) ||
        !yaml_append_nested_flow_comment_fragment(
            wrapped.data, 0, wrapped.length, 4, &formatted_sequence) ||
        formatted_sequence.length < sizeof prefix - 1 ||
        memcmp(formatted_sequence.data, prefix, sizeof prefix - 1) != 0 ||
        !csemver_buffer_append(
            output, formatted_sequence.data + sizeof prefix - 1,
            formatted_sequence.length - (sizeof prefix - 1))) {
      csemver_buffer_free(&formatted_sequence);
      csemver_buffer_free(&wrapped);
      return 0;
    }
    csemver_buffer_free(&formatted_sequence);
    csemver_buffer_free(&wrapped);
    return 1;
  }
  if (content[start] != '{')
    return 0;
  fragment = malloc(end - start + 1);
  if (fragment == NULL)
    return 0;
  memcpy(fragment, content + start, end - start);
  fragment[end - start] = '\0';
  if (!yaml_format_multiline_root_flow(fragment, &formatted, &formatted_size,
                                       &handled) ||
      !handled) {
    free(formatted);
    free(fragment);
    return 0;
  }
  while (position < formatted_size) {
    size_t line_end = position;
    while (line_end < formatted_size && formatted[line_end] != '\n')
      ++line_end;
    if ((line_index > 0 && !csemver_buffer_append(output, "    ", 4)) ||
        !csemver_buffer_append(output, formatted + position,
                               line_end - position) ||
        (line_end < formatted_size &&
         !csemver_buffer_append(output, "\n", 1))) {
      free(formatted);
      free(fragment);
      return 0;
    }
    if (line_end < formatted_size)
      position = line_end + 1;
    else
      position = formatted_size;
    ++line_index;
  }
  free(formatted);
  free(fragment);
  return 1;
}

static int yaml_format_nested_flow_sequence(
    const char *fragment, size_t first_entry_start,
    size_t collection_close_start, const YamlFlowSeparator *separators,
    size_t separator_count, char **formatted, size_t *formatted_size) {
  CsemverBuffer sequence;
  size_t item_start = first_entry_start;
  size_t separator_index;
  csemver_buffer_init(&sequence);
  if (!csemver_buffer_append(&sequence, "[\n  ", 4))
    goto fail;
  for (separator_index = 0; separator_index < separator_count;
       ++separator_index) {
    const YamlFlowSeparator *separator = &separators[separator_index];
    size_t comment_start = SIZE_MAX;
    size_t comment_end;
    size_t index;
    bool comment_on_new_line = false;
    if (separator->start < item_start || separator->end < separator->start ||
        separator->next_start < separator->end ||
        separator->next_start > collection_close_start)
      goto fail;
    if (!yaml_append_normalized_flow_fragment(fragment, item_start,
                                              separator->start, &sequence) &&
        !yaml_append_nested_flow_collection_item(fragment, item_start,
                                                 separator->start, &sequence))
      goto fail;
    if (!csemver_buffer_append(&sequence, ",", 1))
      goto fail;
    for (index = separator->end; index < separator->next_start; ++index) {
      if (fragment[index] == '#') {
        comment_start = index;
        break;
      }
    }
    if (comment_start == SIZE_MAX) {
      for (index = separator->end; index < separator->next_start; ++index) {
        if (!yaml_flow_whitespace(fragment[index]))
          goto fail;
      }
    } else {
      for (index = separator->end; index < comment_start; ++index) {
        if (!yaml_flow_whitespace(fragment[index]))
          goto fail;
        if (fragment[index] == '\r' || fragment[index] == '\n')
          comment_on_new_line = true;
      }
      index = comment_start;
      for (;;) {
        while (index < separator->next_start && fragment[index] != '\r' &&
               fragment[index] != '\n')
          ++index;
        comment_end = index;
        while (index < separator->next_start &&
               yaml_flow_whitespace(fragment[index]))
          ++index;
        if (index < separator->next_start && fragment[index] == '#')
          continue;
        if (index < separator->next_start)
          goto fail;
        break;
      }
      if (!yaml_append_flow_comment_block(&sequence, fragment, comment_start,
                                          comment_end - comment_start,
                                          comment_on_new_line))
        goto fail;
    }
    if (!csemver_buffer_append(&sequence, "\n  ", 3))
      goto fail;
    item_start = separator->next_start;
  }
  if (item_start >= collection_close_start)
    goto fail;
  if (!yaml_append_normalized_flow_fragment(
          fragment, item_start, collection_close_start, &sequence) &&
      !yaml_append_nested_flow_collection_item(
          fragment, item_start, collection_close_start, &sequence))
    goto fail;
  if (!csemver_buffer_append(&sequence, "\n]", 2))
    goto fail;
  *formatted = sequence.data;
  *formatted_size = sequence.length;
  return 1;
fail:
  csemver_buffer_free(&sequence);
  return 0;
}

static int yaml_append_nested_flow_comment_fragment(const char *content,
                                                    size_t start, size_t end,
                                                    size_t indent_spaces,
                                                    CsemverBuffer *output) {
  const char *fragment = content + start;
  size_t length;
  size_t flow_depth = 0;
  size_t collection_start = SIZE_MAX;
  size_t collection_open_end = SIZE_MAX;
  size_t first_entry_start = SIZE_MAX;
  size_t collection_close_start = SIZE_MAX;
  size_t collection_close_end = SIZE_MAX;
  YamlFlowSeparator *sequence_separators = NULL;
  size_t sequence_separator_capacity = 0;
  size_t separator_count = 0;
  size_t comment_start = SIZE_MAX;
  size_t comment_end;
  size_t prefix_end;
  size_t previous_token_end = 0;
  size_t index;
  bool has_nested_collection = false;
  bool has_comment = false;
  bool collection_is_sequence = false;
  bool separator_pending = false;
  bool failed = false;
  yaml_parser_t parser;
  if (end < start)
    return 0;
  length = end - start;
  if (!yaml_parser_initialize(&parser))
    return 0;
  yaml_parser_set_input_string(&parser, (const unsigned char *)fragment,
                               length);
  for (;;) {
    yaml_token_t token;
    yaml_token_type_t type;
    bool done;
    size_t token_start;
    size_t token_end;
    if (!yaml_parser_scan(&parser, &token)) {
      failed = true;
      break;
    }
    type = token.type;
    done = type == YAML_STREAM_END_TOKEN;
    if (!yaml_mark_to_byte_offset(fragment, token.start_mark.index,
                                  &token_start) ||
        !yaml_mark_to_byte_offset(fragment, token.end_mark.index, &token_end)) {
      failed = true;
    } else {
      if (flow_depth > 0) {
        for (index = previous_token_end; index < token_start; ++index) {
          if (fragment[index] == '#') {
            has_comment = true;
            break;
          }
        }
      }
      previous_token_end = token_end;
      if (separator_pending && flow_depth == 1 &&
          type != YAML_FLOW_ENTRY_TOKEN &&
          type != YAML_FLOW_MAPPING_END_TOKEN &&
          type != YAML_FLOW_SEQUENCE_END_TOKEN && !done) {
        if (collection_is_sequence)
          sequence_separators[separator_count - 1].next_start = token_start;
        separator_pending = false;
      }
      if (collection_start != SIZE_MAX && flow_depth == 1 &&
          first_entry_start == SIZE_MAX && type != YAML_FLOW_ENTRY_TOKEN &&
          type != YAML_FLOW_MAPPING_END_TOKEN &&
          type != YAML_FLOW_SEQUENCE_END_TOKEN && !done)
        first_entry_start = token_start;
      if (collection_start != SIZE_MAX && flow_depth == 1 &&
          type == YAML_FLOW_ENTRY_TOKEN) {
        size_t separator_position = separator_count++;
        if (collection_is_sequence) {
          if (separator_count > sequence_separator_capacity) {
            size_t new_capacity = sequence_separator_capacity == 0
                                      ? 8
                                      : sequence_separator_capacity * 2;
            YamlFlowSeparator *new_separators;
            if (new_capacity < sequence_separator_capacity ||
                new_capacity > SIZE_MAX / sizeof *sequence_separators) {
              failed = true;
            } else {
              new_separators =
                  realloc(sequence_separators,
                          new_capacity * sizeof *sequence_separators);
              if (new_separators == NULL)
                failed = true;
              else {
                sequence_separators = new_separators;
                sequence_separator_capacity = new_capacity;
              }
            }
          }
          if (!failed) {
            sequence_separators[separator_position].start = token_start;
            sequence_separators[separator_position].end = token_end;
            sequence_separators[separator_position].next_start = SIZE_MAX;
            sequence_separators[separator_position].comment_start = SIZE_MAX;
            sequence_separators[separator_position].comment_length = 0;
            sequence_separators[separator_position].comment_on_new_line = false;
          }
        }
        separator_pending = true;
      }
      if (type == YAML_FLOW_MAPPING_START_TOKEN ||
          type == YAML_FLOW_SEQUENCE_START_TOKEN) {
        if (flow_depth == 0 && collection_start == SIZE_MAX) {
          collection_start = token_start;
          collection_open_end = token_end;
          collection_is_sequence = type == YAML_FLOW_SEQUENCE_START_TOKEN;
        } else if (flow_depth > 0) {
          has_nested_collection = true;
        }
        ++flow_depth;
      } else if (type == YAML_FLOW_MAPPING_END_TOKEN ||
                 type == YAML_FLOW_SEQUENCE_END_TOKEN) {
        if (flow_depth == 1 && collection_start != SIZE_MAX &&
            ((collection_is_sequence && type == YAML_FLOW_SEQUENCE_END_TOKEN) ||
             (!collection_is_sequence &&
              type == YAML_FLOW_MAPPING_END_TOKEN))) {
          collection_close_start = token_start;
          collection_close_end = token_end;
        }
        if (flow_depth > 0)
          --flow_depth;
      }
    }
    yaml_token_delete(&token);
    if (failed || done)
      break;
  }
  yaml_parser_delete(&parser);
  if (failed || collection_start == SIZE_MAX ||
      collection_open_end == SIZE_MAX || first_entry_start == SIZE_MAX ||
      collection_close_start == SIZE_MAX || collection_close_end == SIZE_MAX ||
      collection_start < 1 || collection_open_end > first_entry_start ||
      first_entry_start > collection_close_start ||
      collection_close_end > length) {
    free(sequence_separators);
    return 0;
  }
  prefix_end = collection_start;
  while (prefix_end > 0 &&
         (fragment[prefix_end - 1] == ' ' || fragment[prefix_end - 1] == '\t'))
    --prefix_end;
  if (prefix_end == 0 || fragment[prefix_end - 1] != ':') {
    free(sequence_separators);
    return 0;
  }
  for (index = collection_close_end; index < length; ++index) {
    if (!yaml_flow_whitespace(fragment[index])) {
      free(sequence_separators);
      return 0;
    }
  }
  if (separator_count > 0 || (!collection_is_sequence && has_comment) ||
      (collection_is_sequence && has_comment && has_nested_collection)) {
    char *formatted_collection = NULL;
    size_t formatted_collection_size = 0;
    size_t position = 0;
    if (!has_comment) {
      free(sequence_separators);
      return 0;
    }
    if (collection_is_sequence) {
      int formatted = yaml_format_nested_flow_sequence(
          fragment, first_entry_start, collection_close_start,
          sequence_separators, separator_count, &formatted_collection,
          &formatted_collection_size);
      free(sequence_separators);
      sequence_separators = NULL;
      if (!formatted)
        return 0;
    } else {
      char *nested_input;
      size_t nested_length = collection_close_end - collection_start;
      bool handled = false;
      if (nested_length == SIZE_MAX)
        return 0;
      nested_input = malloc(nested_length + 1);
      if (nested_input == NULL)
        return 0;
      memcpy(nested_input, fragment + collection_start, nested_length);
      nested_input[nested_length] = '\0';
      if (!yaml_format_multiline_root_flow(nested_input, &formatted_collection,
                                           &formatted_collection_size,
                                           &handled) ||
          !handled) {
        free(nested_input);
        free(formatted_collection);
        return 0;
      }
      free(nested_input);
    }
    if (!csemver_buffer_append(output, fragment, prefix_end) ||
        !csemver_buffer_append(output, "\n", 1)) {
      free(formatted_collection);
      return 0;
    }
    while (position < formatted_collection_size) {
      size_t line_end = position;
      while (line_end < formatted_collection_size &&
             formatted_collection[line_end] != '\n')
        ++line_end;
      if (!csemver_buffer_append(output, "    ", indent_spaces) ||
          !csemver_buffer_append(output, formatted_collection + position,
                                 line_end - position)) {
        free(formatted_collection);
        return 0;
      }
      if (line_end < formatted_collection_size) {
        if (!csemver_buffer_append(output, "\n", 1)) {
          free(formatted_collection);
          return 0;
        }
        position = line_end + 1;
      } else {
        position = formatted_collection_size;
      }
    }
    free(formatted_collection);
    return 1;
  }
  if (collection_is_sequence)
    return 0;
  for (index = collection_open_end; index < first_entry_start; ++index) {
    if (fragment[index] == '#') {
      comment_start = index;
      break;
    }
  }
  if (comment_start == SIZE_MAX)
    return 0;
  for (index = collection_open_end; index < comment_start; ++index) {
    if (!yaml_flow_whitespace(fragment[index]))
      return 0;
  }
  comment_end = comment_start;
  while (comment_end < first_entry_start && fragment[comment_end] != '\r' &&
         fragment[comment_end] != '\n')
    ++comment_end;
  for (index = comment_end; index < first_entry_start; ++index) {
    if (!yaml_flow_whitespace(fragment[index]))
      return 0;
  }
  if (!csemver_buffer_append(output, fragment, prefix_end) ||
      !csemver_buffer_append(output, "\n    {\n      ", 13) ||
      !csemver_buffer_append(output, fragment + comment_start,
                             comment_end - comment_start) ||
      !csemver_buffer_append(output, "\n      ", 7) ||
      !yaml_append_normalized_flow_fragment(fragment, first_entry_start,
                                            collection_close_start, output) ||
      !csemver_buffer_append(output, "\n    }", 6))
    return 0;
  return 1;
}

static int yaml_format_multiline_root_flow(const char *content, char **output,
                                           size_t *output_size, bool *handled) {
  yaml_parser_t parser;
  YamlFlowSeparator *separators = NULL;
  size_t separator_count = 0;
  size_t separator_capacity = 0;
  size_t flow_depth = 0;
  size_t pending_separator = SIZE_MAX;
  size_t previous_token_end = 0;
  size_t root_start = 0;
  size_t root_open_end = 0;
  size_t root_first_entry_start = SIZE_MAX;
  size_t root_close_start = 0;
  size_t root_close_end = 0;
  size_t root_start_line = 0;
  size_t root_close_line = 0;
  bool root_found = false;
  bool root_closed = false;
  bool failed = false;
  bool has_inline_comment = false;
  bool has_nested_comment = false;
  bool has_trailing_comment = false;
  bool has_leading_comment = false;
  bool trailing_comma = false;
  size_t leading_comment_start = 0;
  size_t leading_comment_length = 0;
  size_t trailing_comment_start = 0;
  size_t trailing_comment_length = 0;
  size_t entry_count;
  size_t index;
  CsemverBuffer formatted;

  *output = NULL;
  *output_size = 0;
  *handled = false;
  if (!yaml_parser_initialize(&parser))
    return 0;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content,
                               strlen(content));
  for (;;) {
    yaml_token_t token;
    yaml_token_type_t type;
    bool done;
    size_t start;
    size_t end;
    if (!yaml_parser_scan(&parser, &token)) {
      failed = true;
      break;
    }
    type = token.type;
    done = type == YAML_STREAM_END_TOKEN;
    if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
        !yaml_mark_to_byte_offset(content, token.end_mark.index, &end)) {
      failed = true;
    } else {
      size_t scan;
      if (flow_depth > 1) {
        for (scan = previous_token_end; scan < start; ++scan) {
          if (content[scan] == '#') {
            has_nested_comment = true;
            break;
          }
        }
      }
      previous_token_end = end;
    }
    if (root_found && !root_closed && flow_depth == 1 &&
        root_first_entry_start == SIZE_MAX && type != YAML_FLOW_ENTRY_TOKEN &&
        type != YAML_FLOW_MAPPING_END_TOKEN &&
        type != YAML_FLOW_SEQUENCE_END_TOKEN && !done) {
      if (!yaml_mark_to_byte_offset(content, token.start_mark.index,
                                    &root_first_entry_start))
        failed = true;
    }
    if (pending_separator != SIZE_MAX && flow_depth == 1 &&
        type != YAML_FLOW_ENTRY_TOKEN && type != YAML_FLOW_MAPPING_END_TOKEN &&
        type != YAML_FLOW_SEQUENCE_END_TOKEN && !done) {
      if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start))
        failed = true;
      else {
        separators[pending_separator].next_start = start;
        pending_separator = SIZE_MAX;
      }
    }
    if (type == YAML_FLOW_MAPPING_START_TOKEN ||
        type == YAML_FLOW_SEQUENCE_START_TOKEN) {
      if (flow_depth == 0 && !root_found &&
          type == YAML_FLOW_MAPPING_START_TOKEN) {
        if (!yaml_mark_to_byte_offset(content, token.start_mark.index,
                                      &root_start) ||
            !yaml_mark_to_byte_offset(content, token.end_mark.index,
                                      &root_open_end))
          failed = true;
        else {
          root_found = true;
          root_start_line = token.start_mark.line;
        }
      }
      ++flow_depth;
    } else if (type == YAML_FLOW_MAPPING_END_TOKEN ||
               type == YAML_FLOW_SEQUENCE_END_TOKEN) {
      if (flow_depth == 1 && root_found && !root_closed &&
          type == YAML_FLOW_MAPPING_END_TOKEN) {
        if (!yaml_mark_to_byte_offset(content, token.start_mark.index,
                                      &root_close_start) ||
            !yaml_mark_to_byte_offset(content, token.end_mark.index,
                                      &root_close_end))
          failed = true;
        else {
          root_closed = true;
          root_close_line = token.start_mark.line;
          if (pending_separator != SIZE_MAX) {
            separators[pending_separator].next_start = root_close_start;
            pending_separator = SIZE_MAX;
          }
        }
      }
      if (flow_depth > 0)
        --flow_depth;
    } else if (type == YAML_FLOW_ENTRY_TOKEN && flow_depth == 1 && root_found &&
               !root_closed) {
      if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
          !yaml_mark_to_byte_offset(content, token.end_mark.index, &end)) {
        failed = true;
      } else {
        if (separator_count == separator_capacity) {
          size_t new_capacity =
              separator_capacity == 0 ? 8 : separator_capacity * 2;
          YamlFlowSeparator *new_separators;
          if (new_capacity < separator_capacity ||
              new_capacity > SIZE_MAX / sizeof *separators) {
            failed = true;
          } else {
            new_separators =
                realloc(separators, new_capacity * sizeof *separators);
            if (new_separators == NULL)
              failed = true;
            else {
              separators = new_separators;
              separator_capacity = new_capacity;
            }
          }
        }
        if (!failed) {
          separators[separator_count].start = start;
          separators[separator_count].end = end;
          separators[separator_count].next_start = SIZE_MAX;
          separators[separator_count].comment_start = SIZE_MAX;
          separators[separator_count].comment_length = 0;
          separators[separator_count].comment_on_new_line = false;
          pending_separator = separator_count++;
        }
      }
    }
    yaml_token_delete(&token);
    if (failed || done)
      break;
  }
  yaml_parser_delete(&parser);
  if (failed) {
    free(separators);
    return 0;
  }
  if (!root_found || !root_closed || root_close_line == root_start_line ||
      root_start > root_open_end || root_open_end > root_close_start ||
      root_close_start > root_close_end || root_first_entry_start == SIZE_MAX ||
      root_first_entry_start < root_open_end ||
      root_first_entry_start > root_close_start) {
    free(separators);
    return 1;
  }
  for (index = 0; index < root_start; ++index) {
    if (!yaml_flow_whitespace(content[index])) {
      free(separators);
      return 1;
    }
  }
  for (index = root_open_end; index < root_first_entry_start; ++index) {
    if (content[index] == '#') {
      size_t comment_end = index;
      size_t trailing;
      while (comment_end < root_first_entry_start &&
             content[comment_end] != '\r' && content[comment_end] != '\n')
        ++comment_end;
      for (trailing = root_open_end; trailing < index; ++trailing) {
        if (!yaml_flow_whitespace(content[trailing])) {
          free(separators);
          return 1;
        }
      }
      for (trailing = comment_end; trailing < root_first_entry_start;
           ++trailing) {
        if (content[trailing] == '#') {
          free(separators);
          return 1;
        }
        if (!yaml_flow_whitespace(content[trailing])) {
          free(separators);
          return 1;
        }
      }
      leading_comment_start = index;
      leading_comment_length = comment_end - index;
      has_leading_comment = true;
      has_inline_comment = true;
      break;
    }
  }
  trailing_comma =
      separator_count > 0 &&
      separators[separator_count - 1].next_start == root_close_start;
  entry_count = separator_count + (trailing_comma ? 0 : 1);
  for (index = 0; index < separator_count; ++index) {
    size_t position;
    YamlFlowSeparator *separator = &separators[index];
    if (separator->next_start == SIZE_MAX ||
        separator->next_start < separator->end) {
      free(separators);
      return 1;
    }
    for (position = separator->end; position < separator->next_start;
         ++position) {
      if (content[position] == '#') {
        size_t comment_end = position;
        size_t scan = position;
        size_t trailing;
        while (scan < separator->next_start) {
          while (scan < separator->next_start && content[scan] != '\r' &&
                 content[scan] != '\n')
            ++scan;
          comment_end = scan;
          while (scan < separator->next_start &&
                 yaml_flow_whitespace(content[scan]))
            ++scan;
          if (scan < separator->next_start && content[scan] == '#')
            continue;
          break;
        }
        separator->comment_start = position;
        separator->comment_length = comment_end - position;
        for (trailing = separator->end; trailing < position; ++trailing) {
          if (content[trailing] == '\r' || content[trailing] == '\n') {
            separator->comment_on_new_line = true;
            break;
          }
        }
        if (trailing_comma && index + 1 == separator_count) {
          trailing_comment_start = position;
          trailing_comment_length = comment_end - position;
          has_trailing_comment = true;
        } else {
          has_inline_comment = true;
        }
        break;
      }
    }
  }
  if (has_nested_comment)
    has_inline_comment = true;
  csemver_buffer_init(&formatted);
  if (!csemver_buffer_append(&formatted, content, root_start) ||
      !csemver_buffer_append(&formatted, has_inline_comment ? "{\n" : "{ ", 2))
    goto allocation_error;
  if (has_leading_comment &&
      (!csemver_buffer_append(&formatted, "  ", 2) ||
       !csemver_buffer_append(&formatted, content + leading_comment_start,
                              leading_comment_length) ||
       !csemver_buffer_append(&formatted, "\n", 1)))
    goto allocation_error;
  for (index = 0; index < entry_count; ++index) {
    size_t entry_start =
        index == 0 ? root_first_entry_start : separators[index - 1].next_start;
    size_t entry_end =
        index < separator_count ? separators[index].start : root_close_start;
    bool final_trailing_separator =
        trailing_comma && index + 1 == separator_count;
    if ((has_inline_comment && !csemver_buffer_append(&formatted, "  ", 2)) ||
        (!has_inline_comment && index > 0 &&
         !csemver_buffer_append(&formatted, " ", 1)))
      goto allocation_error;
    if (!yaml_append_normalized_flow_fragment(content, entry_start, entry_end,
                                              &formatted)) {
      CsemverBuffer nested;
      csemver_buffer_init(&nested);
      if (!yaml_append_nested_flow_comment_fragment(content, entry_start,
                                                    entry_end, 4, &nested)) {
        csemver_buffer_free(&nested);
        goto unsupported;
      }
      if (!csemver_buffer_append(&formatted, nested.data, nested.length)) {
        csemver_buffer_free(&nested);
        goto allocation_error;
      }
      csemver_buffer_free(&nested);
    }
    if (index < separator_count && !final_trailing_separator) {
      YamlFlowSeparator *separator = &separators[index];
      if (!csemver_buffer_append(&formatted, ",", 1))
        goto allocation_error;
      if (separator->comment_length > 0 &&
          !yaml_append_flow_comment_block(
              &formatted, content, separator->comment_start,
              separator->comment_length, separator->comment_on_new_line))
        goto allocation_error;
    }
    if (has_inline_comment && !csemver_buffer_append(&formatted, "\n", 1))
      goto allocation_error;
  }
  if (!csemver_buffer_append(&formatted, has_inline_comment ? "}" : " }",
                             has_inline_comment ? 1 : 2) ||
      (has_trailing_comment &&
       (!csemver_buffer_append(&formatted, " ", 1) ||
        !csemver_buffer_append(&formatted, content + trailing_comment_start,
                               trailing_comment_length))) ||
      !csemver_buffer_append(&formatted, content + root_close_end,
                             strlen(content) - root_close_end))
    goto allocation_error;
  free(separators);
  *output = formatted.data;
  *output_size = formatted.length;
  *handled = true;
  return 1;
unsupported:
  csemver_buffer_free(&formatted);
  free(separators);
  return 1;
allocation_error:
  csemver_buffer_free(&formatted);
  free(separators);
  return 0;
}

static void yaml_trim_trailing_horizontal_space(CsemverBuffer *buffer) {
  while (buffer->length > 0 && (buffer->data[buffer->length - 1] == ' ' ||
                                buffer->data[buffer->length - 1] == '\t'))
    --buffer->length;
  if (buffer->data != NULL)
    buffer->data[buffer->length] = '\0';
}

static void yaml_trim_leading_blank_lines(CsemverBuffer *buffer) {
  size_t position = 0;
  while (position < buffer->length) {
    size_t line_end = position;
    while (line_end < buffer->length &&
           (buffer->data[line_end] == ' ' || buffer->data[line_end] == '\t'))
      ++line_end;
    if (line_end == buffer->length ||
        (buffer->data[line_end] != '\r' && buffer->data[line_end] != '\n'))
      break;
    if (buffer->data[line_end] == '\r' && line_end + 1 < buffer->length &&
        buffer->data[line_end + 1] == '\n')
      position = line_end + 2;
    else
      position = line_end + 1;
  }
  if (position > 0) {
    memmove(buffer->data, buffer->data + position, buffer->length - position);
    buffer->length -= position;
    buffer->data[buffer->length] = '\0';
  }
}

static int yaml_separate_trailing_root_comments(CsemverBuffer *buffer) {
  size_t position = 0;
  size_t content_end = 0;
  size_t comment_start = 0;
  size_t index;
  bool have_content = false;
  bool has_trailing_comments = false;
  bool last_content_is_document_end = false;
  const char *newline = "\n";
  size_t newline_length = 1;
  CsemverBuffer separated;

  for (index = 0; index + 1 < buffer->length; ++index) {
    if (buffer->data[index] == '\r' && buffer->data[index + 1] == '\n') {
      newline = "\r\n";
      newline_length = 2;
      break;
    }
    if (buffer->data[index] == '\n')
      break;
  }
  while (position < buffer->length) {
    size_t line_start = position;
    size_t line_end = position;
    size_t next_line;
    size_t first_nonspace = line_start;
    bool blank;
    while (line_end < buffer->length && buffer->data[line_end] != '\r' &&
           buffer->data[line_end] != '\n')
      ++line_end;
    next_line = line_end;
    if (next_line < buffer->length && buffer->data[next_line] == '\r')
      ++next_line;
    if (next_line < buffer->length && buffer->data[next_line] == '\n')
      ++next_line;
    while (first_nonspace < line_end && (buffer->data[first_nonspace] == ' ' ||
                                         buffer->data[first_nonspace] == '\t'))
      ++first_nonspace;
    blank = first_nonspace == line_end;
    if (!blank && buffer->data[line_start] == '#') {
      if (have_content) {
        if (!has_trailing_comments)
          comment_start = line_start;
        has_trailing_comments = true;
      }
    } else if (!blank) {
      size_t marker_end = line_end;
      while (marker_end > first_nonspace &&
             (buffer->data[marker_end - 1] == ' ' ||
              buffer->data[marker_end - 1] == '\t'))
        --marker_end;
      have_content = true;
      content_end = line_end;
      has_trailing_comments = false;
      last_content_is_document_end =
          marker_end - first_nonspace == 3 &&
          memcmp(buffer->data + first_nonspace, "...", 3) == 0;
    }
    position = next_line;
  }
  if (!has_trailing_comments || !have_content || last_content_is_document_end)
    return 1;
  csemver_buffer_init(&separated);
  if (!csemver_buffer_append(&separated, buffer->data, content_end) ||
      !csemver_buffer_append(&separated, newline, newline_length) ||
      !csemver_buffer_append(&separated, newline, newline_length) ||
      !csemver_buffer_append(&separated, buffer->data + comment_start,
                             buffer->length - comment_start)) {
    csemver_buffer_free(&separated);
    return 0;
  }
  csemver_buffer_free(buffer);
  *buffer = separated;
  return 1;
}

static int yaml_match_serialized_newline(const char *source,
                                         CsemverBuffer *output) {
  size_t source_length = strlen(source);
  size_t crlf_count = 0;
  size_t lf_count = 0;
  size_t index;
  bool has_line_ending;
  bool output_has_terminal_line_ending =
      output->length > 0 && (output->data[output->length - 1] == '\n' ||
                             output->data[output->length - 1] == '\r');
  const char *replacement;
  size_t replacement_length;
  CsemverBuffer normalized;
  for (index = 0; index < source_length; ++index) {
    if (source[index] == '\n') {
      if (index > 0 && source[index - 1] == '\r')
        ++crlf_count;
      else
        ++lf_count;
    }
  }
  has_line_ending = crlf_count + lf_count > 0;
  replacement = has_line_ending && crlf_count > lf_count ? "\r\n" : "\n";
  replacement_length = has_line_ending && crlf_count > lf_count ? 2 : 1;
  csemver_buffer_init(&normalized);
  for (index = 0; index < output->length;) {
    char byte = output->data[index];
    if (byte == '\r' || byte == '\n') {
      if (byte == '\r' && index + 1 < output->length &&
          output->data[index + 1] == '\n')
        index += 2;
      else
        ++index;
      if (has_line_ending) {
        if (!csemver_buffer_append(&normalized, replacement,
                                   replacement_length))
          goto allocation_error;
      } else if (!csemver_buffer_append(&normalized, "undefined", 9)) {
        goto allocation_error;
      }
    } else {
      if (!csemver_buffer_append(&normalized, &byte, 1))
        goto allocation_error;
      ++index;
    }
  }
  if (!output_has_terminal_line_ending &&
      !csemver_buffer_append(&normalized,
                             has_line_ending ? replacement : "undefined",
                             has_line_ending ? replacement_length : 9))
    goto allocation_error;
  csemver_buffer_free(output);
  *output = normalized;
  return 1;
allocation_error:
  csemver_buffer_free(&normalized);
  return 0;
}

static int yaml_normalize_single_line_flow(const char *content, char **output,
                                           size_t *output_size) {
  size_t length = strlen(content);
  size_t line_length = length;
  size_t position = 0;
  size_t depth = 0;
  size_t capacity = 0;
  bool failed = false;
  yaml_parser_t parser;
  YamlFlowFrame *frames = NULL;
  CsemverBuffer buffer;
  if (line_length > 0 && content[line_length - 1] == '\n') {
    --line_length;
    if (line_length > 0 && content[line_length - 1] == '\r')
      --line_length;
  }
  if (memchr(content, '\n', line_length) != NULL ||
      memchr(content, '\r', line_length) != NULL) {
    char *formatted = NULL;
    size_t formatted_size = 0;
    bool handled = false;
    csemver_buffer_init(&buffer);
    if (!yaml_format_multiline_root_flow(content, &formatted, &formatted_size,
                                         &handled)) {
      csemver_buffer_free(&buffer);
      return 0;
    }
    if (!handled) {
      CsemverBuffer nested;
      csemver_buffer_init(&nested);
      if (yaml_append_nested_flow_comment_fragment(content, 0, length, 2,
                                                   &nested)) {
        formatted = nested.data;
        formatted_size = nested.length;
        handled = true;
      } else {
        csemver_buffer_free(&nested);
      }
    }
    if (handled ? !csemver_buffer_append(&buffer, formatted, formatted_size)
                : !csemver_buffer_append(&buffer, content, length)) {
      free(formatted);
      csemver_buffer_free(&buffer);
      return 0;
    }
    yaml_trim_leading_blank_lines(&buffer);
    if (!yaml_match_serialized_newline(content, &buffer) ||
        !yaml_separate_trailing_root_comments(&buffer)) {
      free(formatted);
      csemver_buffer_free(&buffer);
      return 0;
    }
    free(formatted);
    *output = buffer.data;
    *output_size = buffer.length;
    return 1;
  }
  if (!yaml_parser_initialize(&parser))
    return 0;
  yaml_parser_set_input_string(&parser, (const unsigned char *)content, length);
  csemver_buffer_init(&buffer);
  for (;;) {
    yaml_token_t token;
    yaml_token_type_t type;
    bool done;
    if (!yaml_parser_scan(&parser, &token)) {
      failed = true;
      break;
    }
    type = token.type;
    done = type == YAML_STREAM_END_TOKEN;
    if (type == YAML_FLOW_MAPPING_START_TOKEN ||
        type == YAML_FLOW_SEQUENCE_START_TOKEN) {
      size_t start;
      size_t end;
      char opener = type == YAML_FLOW_MAPPING_START_TOKEN ? '{' : '[';
      char closer = type == YAML_FLOW_MAPPING_START_TOKEN ? '}' : ']';
      size_t next;
      bool empty;
      if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
          !yaml_mark_to_byte_offset(content, token.end_mark.index, &end) ||
          start < position || end < start ||
          !csemver_buffer_append(&buffer, content + position,
                                 start - position) ||
          !csemver_buffer_append(&buffer, &opener, 1)) {
        failed = true;
      } else {
        if (depth == capacity) {
          size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
          YamlFlowFrame *new_frames;
          if (new_capacity < capacity ||
              new_capacity > SIZE_MAX / sizeof *frames) {
            failed = true;
          } else {
            new_frames = realloc(frames, new_capacity * sizeof *frames);
            if (new_frames == NULL)
              failed = true;
            else {
              frames = new_frames;
              capacity = new_capacity;
            }
          }
        }
        if (!failed) {
          frames[depth++].output_start = buffer.length - 1;
          position = end;
          next = position;
          while (next < line_length &&
                 (content[next] == ' ' || content[next] == '\t'))
            ++next;
          empty = next < line_length && content[next] == closer;
          position = next;
          if (!empty && !csemver_buffer_append(&buffer, " ", 1))
            failed = true;
        }
      }
    } else if (type == YAML_FLOW_MAPPING_END_TOKEN ||
               type == YAML_FLOW_SEQUENCE_END_TOKEN) {
      size_t start;
      size_t end;
      char closer = type == YAML_FLOW_MAPPING_END_TOKEN ? '}' : ']';
      if (depth == 0 ||
          !yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
          !yaml_mark_to_byte_offset(content, token.end_mark.index, &end) ||
          start < position || end < start ||
          !csemver_buffer_append(&buffer, content + position,
                                 start - position)) {
        failed = true;
      } else {
        yaml_trim_trailing_horizontal_space(&buffer);
        if (buffer.length > frames[depth - 1].output_start + 1 &&
            !csemver_buffer_append(&buffer, " ", 1))
          failed = true;
        if (!failed && !csemver_buffer_append(&buffer, &closer, 1))
          failed = true;
        position = end;
        --depth;
      }
    } else if ((type == YAML_FLOW_ENTRY_TOKEN || type == YAML_VALUE_TOKEN) &&
               depth > 0) {
      size_t start;
      size_t end;
      char separator = type == YAML_FLOW_ENTRY_TOKEN ? ',' : ':';
      if (!yaml_mark_to_byte_offset(content, token.start_mark.index, &start) ||
          !yaml_mark_to_byte_offset(content, token.end_mark.index, &end) ||
          start < position || end < start ||
          !csemver_buffer_append(&buffer, content + position,
                                 start - position)) {
        failed = true;
      } else {
        yaml_trim_trailing_horizontal_space(&buffer);
        if (!csemver_buffer_append(&buffer, &separator, 1) ||
            !csemver_buffer_append(&buffer, " ", 1))
          failed = true;
        position = end;
        while (position < line_length &&
               (content[position] == ' ' || content[position] == '\t'))
          ++position;
      }
    }
    yaml_token_delete(&token);
    if (failed || done)
      break;
  }
  yaml_parser_delete(&parser);
  free(frames);
  if (failed || depth != 0 ||
      !csemver_buffer_append(&buffer, content + position, length - position) ||
      !yaml_match_serialized_newline(content, &buffer)) {
    csemver_buffer_free(&buffer);
    return 0;
  }
  *output = buffer.data;
  *output_size = buffer.length;
  return 1;
}

static int python_version_range(const char *content, Range *range,
                                char *version, size_t version_size) {
  static const char keyword[] = "version";
  const char *line = content;
  while (*line != '\0') {
    const char *line_end = strchr(line, '\n');
    const char *limit = line_end == NULL ? line + strlen(line) : line_end;
    const char *regex_limit = memchr(line, '\r', (size_t)(limit - line));
    const char *candidate;
    if (regex_limit == NULL)
      regex_limit = limit;
    for (candidate = line;
         (size_t)(regex_limit - candidate) >= sizeof keyword - 1; ++candidate) {
      const char *cursor;
      const char *value_start;
      const char *last_quote = NULL;
      const char *search;
      size_t value_length;
      size_t index;
      for (index = 0; index < sizeof keyword - 1; ++index)
        if (tolower((unsigned char)candidate[index]) != keyword[index])
          break;
      if (index != sizeof keyword - 1)
        continue;
      cursor = candidate + sizeof keyword - 1;
      while (cursor < regex_limit && (*cursor == ' ' || *cursor == '"'))
        ++cursor;
      if (cursor >= regex_limit || *cursor++ != '=')
        continue;
      while (cursor < regex_limit && *cursor == ' ')
        ++cursor;
      if (cursor >= regex_limit || (*cursor != '\'' && *cursor != '"'))
        continue;
      value_start = cursor + 1;
      for (cursor = value_start; cursor < regex_limit; ++cursor)
        if (*cursor == '\'' || *cursor == '"')
          last_quote = cursor;
      if (last_quote == NULL)
        continue;
      value_length = (size_t)(last_quote - value_start);
      if (version != NULL) {
        if (value_length >= version_size)
          return 0;
        memcpy(version, value_start, value_length);
        version[value_length] = '\0';
      }
      if (range != NULL) {
        if (value_length == 0) {
          range->start = (size_t)(line - content);
          range->end = range->start;
        } else {
          for (search = line; (size_t)(limit - search) >= value_length;
               ++search)
            if (memcmp(search, value_start, value_length) == 0)
              break;
          if ((size_t)(limit - search) < value_length)
            return 0;
          range->start = (size_t)(search - content);
          range->end = range->start + value_length;
        }
      }
      return 1;
    }
    if (line_end == NULL)
      break;
    line = line_end + 1;
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

static int csproj_version_range(const char *content, Range *range,
                                char *version, size_t version_size) {
  static const char open_tag[] = "<Version>";
  static const char close_tag[] = "</Version>";
  const char *line = content;
  while (*line != '\0') {
    const char *end = strpbrk(line, "\r\n");
    const char *limit = end == NULL ? line + strlen(line) : end;
    const char *start = NULL;
    const char *scan;
    for (scan = line; (size_t)(limit - scan) >= sizeof open_tag - 1; ++scan) {
      if (memcmp(scan, open_tag, sizeof open_tag - 1) == 0) {
        start = scan;
        break;
      }
    }
    if (start != NULL) {
      const char *value_start = start + sizeof open_tag - 1;
      const char *close = NULL;
      for (scan = value_start; (size_t)(limit - scan) >= sizeof close_tag - 1;
           ++scan) {
        if (memcmp(scan, close_tag, sizeof close_tag - 1) == 0)
          close = scan;
      }
      if (close != NULL) {
        size_t version_length = (size_t)(close - value_start);
        if (version != NULL) {
          if (version_length >= version_size)
            return 0;
          memcpy(version, value_start, version_length);
          version[version_length] = '\0';
        }
        if (range != NULL) {
          range->start = (size_t)(start - content);
          range->end = (size_t)(close + sizeof close_tag - 1 - content);
        }
        return 1;
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

static int version_filename_ends_with(const char *filename,
                                      const char *suffix) {
  size_t filename_length = strlen(filename);
  size_t suffix_length = strlen(suffix);
  return filename_length >= suffix_length &&
         strcmp(filename + filename_length - suffix_length, suffix) == 0;
}

#define CSEMVER_PATTERN_MAX_GROUPS 64

static int pattern_version_range(const char *content, const char *pattern,
                                 unsigned version_group, Range *range,
                                 char *error, size_t error_size) {
  regex_t expression;
  regmatch_t matches[CSEMVER_PATTERN_MAX_GROUPS];
  int result;

  if (content == NULL || pattern == NULL || pattern[0] == '\0') {
    set_error(error, error_size, "version pattern is empty");
    return 0;
  }
  if (version_group >= CSEMVER_PATTERN_MAX_GROUPS) {
    set_error(error, error_size, "version pattern capture group is too large");
    return 0;
  }
  result = regcomp(&expression, pattern, REG_EXTENDED | REG_NEWLINE);
  if (result != 0) {
    set_error(error, error_size,
              "invalid POSIX extended regular expression for version file");
    return 0;
  }
  if (version_group > expression.re_nsub) {
    regfree(&expression);
    set_error(error, error_size,
              "version pattern capture group does not exist");
    return 0;
  }
  result = regexec(&expression, content, (size_t)version_group + 1, matches, 0);
  regfree(&expression);
  if (result != 0 || matches[version_group].rm_so < 0 ||
      matches[version_group].rm_eo < matches[version_group].rm_so) {
    set_error(error, error_size, "version pattern did not match the file");
    return 0;
  }
  range->start = (size_t)matches[version_group].rm_so;
  range->end = (size_t)matches[version_group].rm_eo;
  if (range->start == range->end) {
    set_error(error, error_size, "version pattern matched an empty version");
    return 0;
  }
  return 1;
}

int csemver_version_read_pattern_text(const char *content, const char *pattern,
                                      unsigned version_group, char *version,
                                      size_t version_size, char *error,
                                      size_t error_size) {
  Range range;
  size_t length;
  if (!pattern_version_range(content, pattern, version_group, &range, error,
                             error_size))
    return 0;
  length = range.end - range.start;
  if (length >= version_size) {
    set_error(error, error_size, "version output buffer too small");
    return 0;
  }
  memcpy(version, content + range.start, length);
  version[length] = '\0';
  return 1;
}

int csemver_version_update_pattern_text(const char *content,
                                        const char *pattern,
                                        unsigned version_group,
                                        const char *new_version, char **updated,
                                        size_t *updated_size, char *old_version,
                                        size_t old_version_size, char *error,
                                        size_t error_size) {
  Range range;
  size_t old_length, content_length;
  const char *replacement = new_version == NULL ? "null" : new_version;
  CsemverBuffer output;

  if (!pattern_version_range(content, pattern, version_group, &range, error,
                             error_size))
    return 0;
  old_length = range.end - range.start;
  if (old_length >= old_version_size) {
    set_error(error, error_size, "version output buffer too small");
    return 0;
  }
  memcpy(old_version, content + range.start, old_length);
  old_version[old_length] = '\0';
  content_length = strlen(content);
  csemver_buffer_init(&output);
  if (!csemver_buffer_append(&output, content, range.start) ||
      !csemver_buffer_append(&output, replacement, strlen(replacement)) ||
      !csemver_buffer_append(&output, content + range.end,
                             content_length - range.end)) {
    csemver_buffer_free(&output);
    set_error(error, error_size, "out of memory updating version file");
    return 0;
  }
  *updated = output.data;
  *updated_size = output.length;
  return 1;
}

int csemver_version_read_text(const char *filename, const char *type,
                              const char *content, char *version,
                              size_t version_size, bool *is_private,
                              char *error, size_t error_size) {
  const char *kind =
      type != NULL && type[0] != '\0'
          ? type
          : (strstr(filename, ".json") != NULL                 ? "json"
             : strstr(filename, "pyproject.toml") != NULL      ? "python"
             : strstr(filename, ".toml") != NULL               ? "toml"
             : strstr(filename, "build.gradle") != NULL        ? "gradle"
             : version_filename_ends_with(filename, ".csproj") ? "csproj"
             : strstr(filename, "pom.xml") != NULL             ? "maven"
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
  if (strcmp(kind, "csproj") == 0) {
    if (csproj_version_range(content, NULL, version, version_size))
      return 1;
    set_error(error, error_size,
              "Failed to read the Version field in your csproj file - is it "
              "present?");
    return 0;
  }
  if (strcmp(kind, "maven") == 0)
    return csemver_maven_read_text(content, version, version_size, error,
                                   error_size);
  if (strcmp(kind, "python") == 0 &&
      python_version_range(content, NULL, version, version_size))
    return 1;
  if (strcmp(kind, "toml") == 0 &&
      line_version(content, "version", false, NULL, version, version_size))
    return 1;
  if (strcmp(kind, "yaml") == 0 &&
      yaml_version_range(content, false, NULL, version, version_size, NULL,
                         NULL, NULL))
    return 1;
  if (strcmp(kind, "openapi") == 0 &&
      yaml_version_range(content, true, NULL, version, version_size, NULL, NULL,
                         NULL))
    return 1;
  if (strcmp(kind, "plain-text") == 0) {
    size_t length = strlen(content);
    if (length >= version_size) {
      set_error(error, error_size, "version output buffer too small");
      return 0;
    }
    memcpy(version, content, length + 1);
    return 1;
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
          : (strstr(filename, ".json") != NULL                 ? "json"
             : strstr(filename, "pyproject.toml") != NULL      ? "python"
             : strstr(filename, ".toml") != NULL               ? "toml"
             : strstr(filename, "build.gradle") != NULL        ? "gradle"
             : version_filename_ends_with(filename, ".csproj") ? "csproj"
             : strstr(filename, "pom.xml") != NULL             ? "maven"
             : strstr(filename, ".yaml") != NULL ||
                     strstr(filename, ".yml") != NULL
                 ? "yaml"
                 : "plain-text");
  if (strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) {
    char *flow_normalized = NULL;
    size_t flow_normalized_size = 0;
    if (!yaml_rewrite_flow_plain_value_with_bare_cr(content, &flow_normalized,
                                                    &flow_normalized_size)) {
      set_error(error, error_size, "out of memory updating version file");
      return 0;
    }
    if (flow_normalized != NULL) {
      int result = csemver_version_update_text(
          filename, type, flow_normalized, new_version, updated, updated_size,
          old_version, old_version_size, error, error_size);
      free(flow_normalized);
      return result;
    }
  }
  if (strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) {
    char *normalized = NULL;
    size_t normalized_size = 0;
    if (!yaml_rewrite_plain_values_with_bare_cr(content, &normalized,
                                                &normalized_size)) {
      set_error(error, error_size, "out of memory updating version file");
      return 0;
    }
    if (normalized != NULL) {
      int result = csemver_version_update_text(
          filename, type, normalized, new_version, updated, updated_size,
          old_version, old_version_size, error, error_size);
      free(normalized);
      return result;
    }
  }
  if (strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) {
    char *comment_sanitized = NULL;
    bool comment_changed = false;
    if (!yaml_sanitize_bare_cr_comment(content, &comment_sanitized,
                                       &comment_changed)) {
      set_error(error, error_size, "out of memory updating version file");
      return 0;
    }
    if (comment_changed) {
      Range range;
      bool duplicate_version_key = false;
      bool openapi = strcmp(kind, "openapi") == 0;
      if (yaml_has_multiple_documents(comment_sanitized) != 0) {
        free(comment_sanitized);
        set_error(error, error_size,
                  "Document with errors cannot be stringified");
        return 0;
      }
      if (yaml_version_range(comment_sanitized, openapi, &range, old_version,
                             old_version_size, NULL, NULL,
                             &duplicate_version_key)) {
        CsemverBuffer comment_output;
        const char *comment_replacement =
            new_version == NULL ? "null" : new_version;
        char *comment_expanded = NULL;
        size_t comment_expanded_size = 0;
        size_t expanded_range_start = 0;
        size_t expanded_range_end;
        if (duplicate_version_key) {
          free(comment_sanitized);
          set_error(error, error_size,
                    "Document with errors cannot be stringified");
          return 0;
        }
        if (!yaml_expand_bare_cr_comment_markers(
                content, range.start, &comment_expanded, &comment_expanded_size,
                &expanded_range_start)) {
          free(comment_sanitized);
          set_error(error, error_size, "out of memory updating version file");
          return 0;
        }
        expanded_range_end = expanded_range_start + range.end - range.start;
        csemver_buffer_init(&comment_output);
        if (!csemver_buffer_append(&comment_output, comment_expanded,
                                   expanded_range_start) ||
            !csemver_buffer_append(&comment_output, comment_replacement,
                                   strlen(comment_replacement)) ||
            !csemver_buffer_append(
                &comment_output, comment_expanded + expanded_range_end,
                comment_expanded_size - expanded_range_end)) {
          csemver_buffer_free(&comment_output);
          free(comment_expanded);
          free(comment_sanitized);
          set_error(error, error_size, "out of memory updating version file");
          return 0;
        }
        free(comment_expanded);
        free(comment_sanitized);
        *updated = comment_output.data;
        *updated_size = comment_output.length;
        return 1;
      }
      free(comment_sanitized);
    }
  }
  const bool replacement_is_null = new_version == NULL;
  const char *replacement = replacement_is_null ? "null" : new_version;
  Range ranges[3];
  Range insertions[2];
  size_t count = 0, insertion_count = 0, i, j, pos = 0,
         length = strlen(content);
  CsemverBuffer buffer;
  if (strcmp(kind, "maven") == 0)
    return csemver_maven_update_text(content, replacement, updated,
                                     updated_size, old_version,
                                     old_version_size, error, error_size);
  if (strcmp(kind, "python") == 0) {
    if (!python_version_range(content, &ranges[count], old_version,
                              old_version_size)) {
      set_error(error, error_size,
                "Cannot read properties of undefined (reading 'replace')");
      return 0;
    }
    ++count;
  }
  if (strcmp(kind, "python") != 0 &&
      !csemver_version_read_text(filename, type, content, old_version,
                                 old_version_size, NULL, error, error_size)) {
    if ((strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) &&
        (yaml_bare_cr_has_stringifier_error_context(content) ||
         yaml_has_bare_cr_stringifier_error(content) > 0)) {
      set_error(error, error_size,
                "Document with errors cannot be stringified");
    }
    return 0;
  }
  if ((strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) &&
      (yaml_bare_cr_has_stringifier_error_context(content) ||
       yaml_has_bare_cr_stringifier_error(content) > 0)) {
    set_error(error, error_size, "Document with errors cannot be stringified");
    return 0;
  }
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
  if (strcmp(kind, "csproj") == 0) {
    static const char open_tag[] = "<Version>";
    static const char close_tag[] = "</Version>";
    Range csproj_range;
    CsemverBuffer csproj_buffer;
    if (!csproj_version_range(content, &csproj_range, NULL, 0))
      goto bad_format;
    csemver_buffer_init(&csproj_buffer);
    if (!csemver_buffer_append(&csproj_buffer, content, csproj_range.start) ||
        !csemver_buffer_append(&csproj_buffer, open_tag, sizeof open_tag - 1) ||
        !csemver_buffer_append(&csproj_buffer, replacement,
                               strlen(replacement)) ||
        !csemver_buffer_append(&csproj_buffer, close_tag,
                               sizeof close_tag - 1) ||
        !csemver_buffer_append(&csproj_buffer, content + csproj_range.end,
                               length - csproj_range.end) ||
        !csemver_buffer_append(&csproj_buffer, "", 0)) {
      csemver_buffer_free(&csproj_buffer);
      set_error(error, error_size, "out of memory updating version file");
      return 0;
    }
    *updated = csproj_buffer.data;
    *updated_size = csproj_buffer.length;
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
  if (strcmp(kind, "toml") == 0) {
    if (!line_version(content, "version", false, &ranges[count], old_version,
                      old_version_size))
      goto bad_format;
    ++count;
  } else if (strcmp(kind, "yaml") == 0) {
    bool duplicate_version_key = false;
    if (!yaml_version_range(content, false, &ranges[count], old_version,
                            old_version_size, NULL, NULL,
                            &duplicate_version_key))
      goto bad_format;
    if (duplicate_version_key) {
      set_error(error, error_size,
                "Document with errors cannot be stringified");
      return 0;
    }
    ++count;
  } else if (strcmp(kind, "openapi") == 0) {
    bool duplicate_version_key = false;
    if (!yaml_version_range(content, true, &ranges[count], old_version,
                            old_version_size, NULL, NULL,
                            &duplicate_version_key))
      goto bad_format;
    if (duplicate_version_key) {
      set_error(error, error_size,
                "Document with errors cannot be stringified");
      return 0;
    }
    ++count;
  } else if (strcmp(kind, "plain-text") == 0) {
    ranges[count++] = (Range){0, length};
  } else if (strcmp(kind, "python") != 0) {
    goto bad_format;
  }
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
  if (strcmp(kind, "yaml") == 0 || strcmp(kind, "openapi") == 0) {
    char *rewritten;
    size_t rewritten_size;
    char *normalized;
    size_t normalized_size;
    if (yaml_has_multiple_documents(buffer.data) != 0) {
      csemver_buffer_free(&buffer);
      set_error(error, error_size,
                "Document with errors cannot be stringified");
      return 0;
    }
    if (!yaml_escape_bare_cr_in_double_quoted_scalars(buffer.data, &rewritten,
                                                      &rewritten_size))
      goto allocation_error;
    if (rewritten != NULL) {
      csemver_buffer_free(&buffer);
      csemver_buffer_init(&buffer);
      if (!csemver_buffer_append(&buffer, rewritten, rewritten_size)) {
        free(rewritten);
        goto allocation_error;
      }
      free(rewritten);
    }
    if (!yaml_normalize_version_line_spacing(&buffer,
                                             strcmp(kind, "openapi") == 0))
      goto allocation_error;
    if (!yaml_rewrite_folded_scalars(buffer.data, &rewritten, &rewritten_size))
      goto allocation_error;
    if (rewritten != NULL) {
      csemver_buffer_free(&buffer);
      csemver_buffer_init(&buffer);
      if (!csemver_buffer_append(&buffer, rewritten, rewritten_size)) {
        free(rewritten);
        goto allocation_error;
      }
      free(rewritten);
    }
    if (!yaml_rewrite_block_sequence_comments(buffer.data, &rewritten,
                                              &rewritten_size))
      goto allocation_error;
    if (rewritten != NULL) {
      csemver_buffer_free(&buffer);
      csemver_buffer_init(&buffer);
      if (!csemver_buffer_append(&buffer, rewritten, rewritten_size)) {
        free(rewritten);
        goto allocation_error;
      }
      free(rewritten);
    }
    if (yaml_flow_sequence_has_leading_comment(buffer.data) != 0) {
      csemver_buffer_free(&buffer);
      set_error(error, error_size,
                "Document with errors cannot be stringified");
      return 0;
    }
    if (!yaml_normalize_single_line_flow(buffer.data, &normalized,
                                         &normalized_size))
      goto allocation_error;
    csemver_buffer_free(&buffer);
    *updated = normalized;
    *updated_size = normalized_size;
    return 1;
  }
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
