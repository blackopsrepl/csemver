#include "version.h"

#include "common.h"
#include "maven.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
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
  bool closes_as_mapping_key;
} YamlFrame;
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

static int yaml_version_range(const char *content, bool openapi, Range *range,
                              char *version, size_t version_size) {
  yaml_parser_t parser;
  YamlFrame *frames = NULL;
  size_t depth = 0;
  size_t capacity = 0;
  bool found = false;
  bool failed = false;
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
      if (depth > 0 && frames[depth - 1].is_mapping) {
        YamlFrame *frame = &frames[depth - 1];
        if (frame->expect_key) {
          frame->key_is_info = openapi && frame->is_root_mapping &&
                               yaml_scalar_equals(&event, "info");
          frame->key_is_version = yaml_scalar_equals(&event, "version") &&
                                  ((openapi && frame->is_info_mapping) ||
                                   (!openapi && frame->is_root_mapping));
          frame->expect_key = false;
        } else {
          if (frame->key_is_version && !found) {
            if (!yaml_scalar_value_range(content, &event, range, version,
                                         version_size))
              failed = true;
            else
              found = true;
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
  return found && !failed;
}

static int yaml_normalize_single_line_flow(const char *content, char **output,
                                           size_t *output_size);

static bool yaml_flow_whitespace(char byte) {
  return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
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

static int yaml_format_multiline_root_flow(const char *content, char **output,
                                           size_t *output_size, bool *handled) {
  yaml_parser_t parser;
  YamlFlowSeparator *separators = NULL;
  size_t separator_count = 0;
  size_t separator_capacity = 0;
  size_t flow_depth = 0;
  size_t pending_separator = SIZE_MAX;
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
      separator_count == 0 || root_start > root_open_end ||
      root_open_end > root_close_start || root_close_start > root_close_end ||
      root_first_entry_start == SIZE_MAX ||
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
        size_t trailing;
        while (comment_end < separator->next_start &&
               content[comment_end] != '\r' && content[comment_end] != '\n')
          ++comment_end;
        for (trailing = comment_end; trailing < separator->next_start;
             ++trailing) {
          if (content[trailing] == '#') {
            free(separators);
            return 1;
          }
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
         !csemver_buffer_append(&formatted, " ", 1)) ||
        !yaml_append_normalized_flow_fragment(content, entry_start, entry_end,
                                              &formatted))
      goto unsupported;
    if (index < separator_count && !final_trailing_separator) {
      YamlFlowSeparator *separator = &separators[index];
      if (!csemver_buffer_append(&formatted, ",", 1))
        goto allocation_error;
      if (separator->comment_length > 0) {
        if (separator->comment_on_new_line) {
          if (!csemver_buffer_append(&formatted, "\n  ", 3))
            goto allocation_error;
        } else if (!csemver_buffer_append(&formatted, " ", 1)) {
          goto allocation_error;
        }
        if (!csemver_buffer_append(&formatted,
                                   content + separator->comment_start,
                                   separator->comment_length))
          goto allocation_error;
      }
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
                                         &handled) ||
        (handled ? !csemver_buffer_append(&buffer, formatted, formatted_size)
                 : !csemver_buffer_append(&buffer, content, length)) ||
        !yaml_match_serialized_newline(content, &buffer)) {
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
      yaml_version_range(content, false, NULL, version, version_size))
    return 1;
  if (strcmp(kind, "openapi") == 0 &&
      yaml_version_range(content, true, NULL, version, version_size))
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
    if (!yaml_version_range(content, false, &ranges[count], old_version,
                            old_version_size))
      goto bad_format;
    ++count;
  } else if (strcmp(kind, "openapi") == 0) {
    if (!yaml_version_range(content, true, &ranges[count], old_version,
                            old_version_size))
      goto bad_format;
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
    char *normalized;
    size_t normalized_size;
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
