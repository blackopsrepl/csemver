#include "semver.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_component(const char **cursor, unsigned long *value) {
  const char *start = *cursor;
  char *end = NULL;
  unsigned long parsed;

  if (!isdigit((unsigned char)*start))
    return 0;
  if (*start == '0' && isdigit((unsigned char)start[1]))
    return 0;
  errno = 0;
  parsed = strtoul(start, &end, 10);
  if (errno == ERANGE || end == start)
    return 0;
  *cursor = end;
  *value = parsed;
  return 1;
}

static int identifier_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '-';
}

static int validate_identifiers(const char *start, size_t length,
                                int prerelease) {
  size_t segment_start = 0;
  size_t index;

  if (length == 0)
    return 0;
  for (index = 0; index <= length; ++index) {
    if (index < length && start[index] != '.') {
      if (!identifier_char(start[index]))
        return 0;
      continue;
    }
    if (index == segment_start)
      return 0;
    if (prerelease && index - segment_start > 1 &&
        start[segment_start] == '0') {
      size_t digit;
      int numeric = 1;
      for (digit = segment_start; digit < index; ++digit) {
        if (!isdigit((unsigned char)start[digit])) {
          numeric = 0;
          break;
        }
      }
      if (numeric)
        return 0;
    }
    segment_start = index + 1;
  }
  return 1;
}

static size_t javascript_whitespace_length(const unsigned char *text,
                                           size_t remaining) {
  if (remaining == 0)
    return 0;
  if (text[0] == 0x20 || (text[0] >= 0x09 && text[0] <= 0x0d))
    return 1;
  if (remaining >= 2 && text[0] == 0xc2 && text[1] == 0xa0)
    return 2;
  if (remaining >= 3 && text[0] == 0xe1 && text[1] == 0x9a && text[2] == 0x80)
    return 3;
  if (remaining >= 3 && text[0] == 0xe2 && text[1] == 0x80 &&
      ((text[2] >= 0x80 && text[2] <= 0x8a) || text[2] == 0xa8 ||
       text[2] == 0xa9 || text[2] == 0xaf))
    return 3;
  if (remaining >= 3 && text[0] == 0xe2 && text[1] == 0x81 && text[2] == 0x9f)
    return 3;
  if (remaining >= 3 && text[0] == 0xe3 && text[1] == 0x80 && text[2] == 0x80)
    return 3;
  if (remaining >= 3 && text[0] == 0xef && text[1] == 0xbb && text[2] == 0xbf)
    return 3;
  return 0;
}

static size_t javascript_whitespace_suffix_length(const unsigned char *text,
                                                  size_t length) {
  size_t index = length > 3 ? length - 3 : 0;
  for (; index < length; ++index) {
    size_t whitespace =
        javascript_whitespace_length(text + index, length - index);
    if (whitespace != 0 && index + whitespace == length)
      return whitespace;
  }
  return 0;
}

static int semver_parse_exact(const char *text, Semver *version) {
  const char *cursor;
  const char *part;
  const char *end;
  size_t length;

  if (text == NULL || version == NULL)
    return 0;
  memset(version, 0, sizeof(*version));
  cursor = text;
  if (*cursor == 'v')
    ++cursor;
  if (!parse_component(&cursor, &version->major) || *cursor++ != '.' ||
      !parse_component(&cursor, &version->minor) || *cursor++ != '.' ||
      !parse_component(&cursor, &version->patch))
    return 0;

  if (*cursor == '-') {
    part = ++cursor;
    while (*cursor != '\0' && *cursor != '+')
      ++cursor;
    length = (size_t)(cursor - part);
    if (!validate_identifiers(part, length, 1) ||
        length >= sizeof(version->prerelease))
      return 0;
    memcpy(version->prerelease, part, length);
    version->prerelease[length] = '\0';
    version->has_prerelease = 1;
  }

  if (*cursor == '+') {
    part = ++cursor;
    end = part + strlen(part);
    length = (size_t)(end - part);
    if (!validate_identifiers(part, length, 0) ||
        length >= sizeof(version->build))
      return 0;
    memcpy(version->build, part, length);
    version->build[length] = '\0';
    version->has_build = 1;
    cursor = end;
  }
  return *cursor == '\0';
}

int semver_parse(const char *text, Semver *version) {
  const unsigned char *bytes;
  size_t total_length;
  size_t start;
  size_t end;
  char *trimmed;
  size_t length;
  int parsed;

  if (text == NULL || version == NULL)
    return 0;
  bytes = (const unsigned char *)text;
  total_length = strlen(text);
  start = 0;
  end = total_length;
  while (start < end) {
    size_t whitespace =
        javascript_whitespace_length(bytes + start, end - start);
    if (whitespace == 0)
      break;
    start += whitespace;
  }
  while (end > start) {
    size_t whitespace =
        javascript_whitespace_suffix_length(bytes + start, end - start);
    if (whitespace == 0)
      break;
    end -= whitespace;
  }
  if (start == 0 && end == total_length)
    return semver_parse_exact(text, version);

  length = end - start;
  trimmed = malloc(length + 1);
  if (trimmed == NULL) {
    memset(version, 0, sizeof(*version));
    return 0;
  }
  memcpy(trimmed, text + start, length);
  trimmed[length] = '\0';
  parsed = semver_parse_exact(trimmed, version);
  free(trimmed);
  return parsed;
}

int semver_clean(const char *text, Semver *version) {
  const unsigned char *bytes;
  size_t length;
  size_t start = 0;
  int parsed;

  if (text == NULL || version == NULL)
    return 0;
  bytes = (const unsigned char *)text;
  length = strlen(text);
  while (start < length) {
    size_t whitespace =
        javascript_whitespace_length(bytes + start, length - start);
    if (whitespace == 0)
      break;
    start += whitespace;
  }
  while (start < length && (text[start] == '=' || text[start] == 'v'))
    ++start;
  parsed = semver_parse(text + start, version);
  if (parsed) {
    version->has_build = 0;
    version->build[0] = '\0';
  }
  return parsed;
}

int semver_format(const Semver *version, char *output, size_t output_size) {
  int written;

  if (version == NULL || output == NULL || output_size == 0)
    return 0;
  written = snprintf(
      output, output_size, "%lu.%lu.%lu%s%s%s%s", version->major,
      version->minor, version->patch, version->has_prerelease ? "-" : "",
      version->has_prerelease ? version->prerelease : "",
      version->has_build ? "+" : "", version->has_build ? version->build : "");
  return written >= 0 && (size_t)written < output_size;
}

static int increment(unsigned long *value) {
  if (*value == ULONG_MAX)
    return 0;
  ++*value;
  return 1;
}

static int increment_prerelease_sequence(Semver *version) {
  const char *source = version->prerelease;
  const char *token_end = source + strlen(source);
  char next[SEMVER_IDENTIFIER_MAX];

  while (token_end > source) {
    const char *token_start = token_end;
    const char *cursor;
    char token[SEMVER_IDENTIFIER_MAX];
    char *parsed_end;
    unsigned long number;
    int numeric = 1;

    while (token_start > source && token_start[-1] != '.')
      --token_start;
    for (cursor = token_start; cursor < token_end; ++cursor)
      if (!isdigit((unsigned char)*cursor)) {
        numeric = 0;
        break;
      }
    if (numeric) {
      size_t token_length = (size_t)(token_end - token_start);
      memcpy(token, token_start, token_length);
      token[token_length] = '\0';
      errno = 0;
      number = strtoul(token, &parsed_end, 10);
      if (errno == ERANGE || parsed_end == token || *parsed_end != '\0' ||
          number == ULONG_MAX)
        return 0;
      ++number;
      if (snprintf(next, sizeof next, "%.*s%lu%s", (int)(token_start - source),
                   source, number, token_end) >= (int)sizeof next)
        return 0;
      strcpy(version->prerelease, next);
      return 1;
    }
    if (token_start == source)
      break;
    token_end = token_start - 1;
  }
  if (snprintf(next, sizeof next, "%s.0", source) >= (int)sizeof next)
    return 0;
  strcpy(version->prerelease, next);
  return 1;
}

static int prerelease_has_numeric_identifier(const char *prerelease,
                                             const char *identifier) {
  size_t identifier_length = strlen(identifier);
  const char *start;
  const char *end;

  if (strncmp(prerelease, identifier, identifier_length) != 0 ||
      (prerelease[identifier_length] != '.' &&
       prerelease[identifier_length] != '\0'))
    return 0;
  start = prerelease + identifier_length;
  if (*start == '.')
    ++start;
  end = start;
  while (*end != '\0' && *end != '.') {
    if (!isdigit((unsigned char)*end))
      return 0;
    ++end;
  }
  return end > start;
}

static int set_prerelease(Semver *version, const char *identifier,
                          int increment_existing) {
  char next[SEMVER_IDENTIFIER_MAX];

  if (identifier != NULL && identifier[0] != '\0') {
    if (!validate_identifiers(identifier, strlen(identifier), 0))
      return 0;
    if (increment_existing && version->has_prerelease) {
      if (!increment_prerelease_sequence(version))
        return 0;
      if (prerelease_has_numeric_identifier(version->prerelease, identifier)) {
        version->has_build = 0;
        version->build[0] = '\0';
        return 1;
      }
    }
    if (snprintf(next, sizeof next, "%s.0", identifier) >= (int)sizeof next)
      return 0;
  } else if (increment_existing && version->has_prerelease) {
    if (!increment_prerelease_sequence(version))
      return 0;
    version->has_build = 0;
    version->build[0] = '\0';
    return 1;
  } else {
    strcpy(next, "0");
  }
  strcpy(version->prerelease, next);
  version->has_prerelease = 1;
  version->has_build = 0;
  version->build[0] = '\0';
  return 1;
}

int semver_bump(const Semver *version, const char *release_type,
                const char *prerelease_id, char *output, size_t output_size) {
  Semver next;
  int is_pre;
  const char *base_type = release_type;

  if (version == NULL || release_type == NULL || output == NULL)
    return 0;
  next = *version;
  next.has_build = 0;
  next.build[0] = '\0';

  if (strncmp(release_type, "pre", 3) == 0 &&
      (strcmp(release_type, "premajor") == 0 ||
       strcmp(release_type, "preminor") == 0 ||
       strcmp(release_type, "prepatch") == 0)) {
    is_pre = 1;
    base_type = release_type + 3;
  } else {
    is_pre = 0;
  }

  if (strcmp(base_type, "major") == 0) {
    if (!(!is_pre && version->has_prerelease) || version->minor != 0 ||
        version->patch != 0) {
      if (!increment(&next.major))
        return 0;
    }
    next.minor = 0;
    next.patch = 0;
    next.has_prerelease = 0;
    next.prerelease[0] = '\0';
  } else if (strcmp(base_type, "minor") == 0) {
    if (!(!is_pre && version->has_prerelease) || version->patch != 0) {
      if (!increment(&next.minor))
        return 0;
    }
    next.patch = 0;
    next.has_prerelease = 0;
    next.prerelease[0] = '\0';
  } else if (strcmp(base_type, "patch") == 0) {
    if (version->has_prerelease && !is_pre) {
      next.has_prerelease = 0;
      next.prerelease[0] = '\0';
    } else {
      if (!increment(&next.patch))
        return 0;
      next.has_prerelease = 0;
      next.prerelease[0] = '\0';
    }
  } else if (strcmp(base_type, "prerelease") != 0) {
    return 0;
  }

  if (is_pre) {
    if (!set_prerelease(&next, prerelease_id, 0))
      return 0;
  } else if (strcmp(release_type, "prerelease") == 0) {
    if (!set_prerelease(&next, prerelease_id, 1))
      return 0;
  }
  return semver_format(&next, output, output_size);
}

static int compare_identifiers(const char *left, const char *right) {
  const char *left_end;
  const char *right_end;
  size_t left_len;
  size_t right_len;
  int left_numeric;
  int right_numeric;

  while (*left != '\0' || *right != '\0') {
    left_end = strchr(left, '.');
    right_end = strchr(right, '.');
    if (left_end == NULL)
      left_end = left + strlen(left);
    if (right_end == NULL)
      right_end = right + strlen(right);
    left_len = (size_t)(left_end - left);
    right_len = (size_t)(right_end - right);
    left_numeric = left_len > 0;
    right_numeric = right_len > 0;
    for (size_t i = 0; i < left_len; ++i)
      if (!isdigit((unsigned char)left[i]))
        left_numeric = 0;
    for (size_t i = 0; i < right_len; ++i)
      if (!isdigit((unsigned char)right[i]))
        right_numeric = 0;
    if (left_numeric && right_numeric) {
      if (left_len != right_len)
        return left_len < right_len ? -1 : 1;
      {
        int compared = strncmp(left, right, left_len);
        if (compared != 0)
          return compared < 0 ? -1 : 1;
      }
    } else if (left_numeric != right_numeric) {
      return left_numeric ? -1 : 1;
    } else {
      size_t common = left_len < right_len ? left_len : right_len;
      int compared = strncmp(left, right, common);
      if (compared != 0)
        return compared < 0 ? -1 : 1;
      if (left_len != right_len)
        return left_len < right_len ? -1 : 1;
    }
    left = *left_end == '.' ? left_end + 1 : left_end;
    right = *right_end == '.' ? right_end + 1 : right_end;
    if (*left == '\0' || *right == '\0')
      break;
  }
  if (*left == '\0' && *right == '\0')
    return 0;
  return *left == '\0' ? -1 : 1;
}

int semver_compare(const char *left, const char *right) {
  Semver lhs;
  Semver rhs;

  if (!semver_parse(left, &lhs) || !semver_parse(right, &rhs))
    return 0;
  if (lhs.major != rhs.major)
    return lhs.major < rhs.major ? -1 : 1;
  if (lhs.minor != rhs.minor)
    return lhs.minor < rhs.minor ? -1 : 1;
  if (lhs.patch != rhs.patch)
    return lhs.patch < rhs.patch ? -1 : 1;
  if (lhs.has_prerelease != rhs.has_prerelease)
    return lhs.has_prerelease ? -1 : 1;
  if (!lhs.has_prerelease)
    return 0;
  return compare_identifiers(lhs.prerelease, rhs.prerelease);
}
