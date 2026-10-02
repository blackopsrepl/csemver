#include "version.h"

#include "common.h"

#include <ctype.h>
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
  Range root_version, private_value, lock_version, lock_package_version;
  bool has_private, is_private, has_lock_version, has_lock_package_version;
} JsonFields;

static void set_error(char *error, size_t size, const char *message) {
  if (error != NULL && size > 0)
    snprintf(error, size, "%s", message);
}

static void spaces(Scanner *s) {
  while (s->position < s->length &&
         isspace((unsigned char)s->text[s->position]))
    ++s->position;
}

static int string_value(Scanner *s, char *out, size_t out_size, Range *range) {
  size_t used = 0;
  if (s->position >= s->length || s->text[s->position] != '"')
    return 0;
  if (range != NULL)
    range->start = s->position;
  ++s->position;
  while (s->position < s->length) {
    char c = s->text[s->position++];
    if (c == '"') {
      if (out != NULL) {
        if (used >= out_size)
          return 0;
        out[used] = '\0';
      }
      if (range != NULL)
        range->end = s->position;
      return 1;
    }
    if (c == '\\') {
      if (s->position >= s->length)
        return 0;
      c = s->text[s->position++];
      if (c == 'u') {
        if (s->position + 4 > s->length)
          return 0;
        s->position += 4;
        c = '?';
      } else if (c == 'n')
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
      out[used++] = c;
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
    if (!string_value(s, NULL, 0, NULL))
      return 0;
    spaces(s);
    if (s->position >= s->length || s->text[s->position++] != ':')
      return 0;
    spaces(s);
    if (!skip_value(s, NULL))
      return 0;
    spaces(s);
    if (s->position < s->length && s->text[s->position] == ',')
      ++s->position;
    else if (s->position >= s->length || s->text[s->position] != '}')
      return 0;
    spaces(s);
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
    if (s->position < s->length && s->text[s->position] == ',')
      ++s->position;
    else if (s->position >= s->length || s->text[s->position] != ']')
      return 0;
    spaces(s);
  }
  if (s->position >= s->length)
    return 0;
  ++s->position;
  return 1;
}
static int skip_value(Scanner *s, Range *range) {
  size_t start = s->position;
  char first;
  if (start >= s->length)
    return 0;
  first = s->text[start];
  if (first == '"')
    return string_value(s, NULL, 0, range);
  if (first == '{' || first == '[') {
    int ok = first == '{' ? skip_object(s) : skip_array(s);
    if (ok && range != NULL) {
      range->start = start;
      range->end = s->position;
    }
    return ok;
  }
  while (s->position < s->length &&
         strchr(",]} \t\r\n", s->text[s->position]) == NULL)
    ++s->position;
  if (s->position == start)
    return 0;
  if (range != NULL) {
    range->start = start;
    range->end = s->position;
  }
  return 1;
}

static int object_field(Scanner *s, const char *wanted, Range *range,
                        char *decoded, size_t decoded_size) {
  if (s->position >= s->length || s->text[s->position++] != '{')
    return 0;
  spaces(s);
  while (s->position < s->length && s->text[s->position] != '}') {
    char key[128];
    if (!string_value(s, key, sizeof key, NULL))
      return 0;
    spaces(s);
    if (s->position >= s->length || s->text[s->position++] != ':')
      return 0;
    spaces(s);
    if (strcmp(key, wanted) == 0)
      return decoded != NULL ? string_value(s, decoded, decoded_size, range)
                             : skip_value(s, range);
    if (!skip_value(s, NULL))
      return 0;
    spaces(s);
    if (s->position < s->length && s->text[s->position] == ',')
      ++s->position;
    spaces(s);
  }
  return 0;
}

static int json_fields(const char *content, const char *filename,
                       JsonFields *fields) {
  Scanner root = {content, 0, strlen(content)};
  Range packages;
  Range raw;
  memset(fields, 0, sizeof(*fields));
  spaces(&root);
  if (!object_field(&root, "version", &fields->root_version, NULL, 0))
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
  root.position = raw.start;
  spaces(&root);
  if (object_field(&root, "version", &fields->lock_package_version, NULL, 0))
    fields->has_lock_package_version = true;
  root.position = 0;
  spaces(&root);
  if (object_field(&root, "version", &fields->lock_version, NULL, 0))
    fields->has_lock_version = true;
  return 1;
}

static int copy_json_string(const char *text, Range range, char *out,
                            size_t out_size) {
  size_t length;
  if (range.end < range.start + 2 || text[range.start] != '"')
    return 0;
  length = range.end - range.start - 2;
  if (length >= out_size)
    return 0;
  memcpy(out, text + range.start + 1, length);
  out[length] = '\0';
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
             : strstr(filename, ".yaml") != NULL ||
                     strstr(filename, ".yml") != NULL
                 ? "yaml"
                 : "plain-text");
  if (is_private != NULL)
    *is_private = false;
  if (strcmp(kind, "json") == 0) {
    JsonFields fields;
    if (!json_fields(content, filename, &fields) ||
        !copy_json_string(content, fields.root_version, version,
                          version_size)) {
      set_error(error, error_size,
                "JSON version file has no root version string");
      return 0;
    }
    if (is_private != NULL)
      *is_private = fields.is_private;
    return 1;
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
             : strstr(filename, ".yaml") != NULL ||
                     strstr(filename, ".yml") != NULL
                 ? "yaml"
                 : "plain-text");
  Range ranges[3];
  size_t count = 0, i, j, pos = 0, length = strlen(content);
  CsemverBuffer buffer;
  bool quote_json = strcmp(kind, "json") == 0;
  if (!csemver_version_read_text(filename, type, content, old_version,
                                 old_version_size, NULL, error, error_size))
    return 0;
  if (quote_json) {
    JsonFields fields;
    if (!json_fields(content, filename, &fields))
      goto bad_format;
    ranges[count++] = fields.root_version;
    if (fields.has_lock_package_version)
      ranges[count++] = fields.lock_package_version;
  } else if (strcmp(kind, "python") == 0 || strcmp(kind, "toml") == 0) {
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
    if (quote_json && !csemver_buffer_append(&buffer, "\"", 1))
      goto allocation_error;
    if (!csemver_buffer_append(&buffer, new_version, strlen(new_version)))
      goto allocation_error;
    if (quote_json && !csemver_buffer_append(&buffer, "\"", 1))
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
