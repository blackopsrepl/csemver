#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "gitignore.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "config.h"

#ifdef FNM_EXTMATCH
#define IGNORE_FNM_FLAGS (FNM_PERIOD | FNM_EXTMATCH)
#else
#define IGNORE_FNM_FLAGS FNM_PERIOD
#endif

static int ignore_path_has_hidden_component(const char *filename) {
  while (*filename != '\0') {
    const char *slash;
    if (*filename == '.')
      return 1;
    slash = strchr(filename, '/');
    if (slash == NULL)
      return 0;
    filename = slash + 1;
  }
  return 0;
}

static int ignore_path_pattern_matches(const char *pattern,
                                       const char *filename) {
  const char *pattern_slash = strchr(pattern, '/');
  const char *filename_slash = strchr(filename, '/');
  size_t pattern_length = pattern_slash == NULL
                              ? strlen(pattern)
                              : (size_t)(pattern_slash - pattern);
  size_t filename_length = filename_slash == NULL
                               ? strlen(filename)
                               : (size_t)(filename_slash - filename);
  if (pattern_length == 2 && memcmp(pattern, "**", 2) == 0) {
    const char *remaining_pattern =
        pattern_slash == NULL ? "" : pattern_slash + 1;
    const char *candidate;
    if (pattern_slash == NULL)
      return !ignore_path_has_hidden_component(filename);
    if (*filename == '.' || (candidate = strchr(filename, '/')) == NULL)
      return 0;
    candidate++;
    for (;;) {
      if (ignore_path_pattern_matches(remaining_pattern, candidate))
        return 1;
      if (*candidate == '.')
        return 0;
      candidate = strchr(candidate, '/');
      if (candidate == NULL)
        return 0;
      ++candidate;
    }
  }
  if ((pattern_slash == NULL) != (filename_slash == NULL))
    return 0;
  char *pattern_component = malloc(pattern_length + 1);
  char *filename_component = malloc(filename_length + 1);
  if (pattern_component == NULL || filename_component == NULL) {
    free(pattern_component);
    free(filename_component);
    return 0;
  }
  memcpy(pattern_component, pattern, pattern_length);
  pattern_component[pattern_length] = '\0';
  memcpy(filename_component, filename, filename_length);
  filename_component[filename_length] = '\0';
  int matched =
      fnmatch(pattern_component, filename_component, IGNORE_FNM_FLAGS) == 0;
  free(pattern_component);
  free(filename_component);
  if (!matched)
    return 0;
  if (pattern_slash == NULL)
    return 1;
  return ignore_path_pattern_matches(pattern_slash + 1, filename_slash + 1);
}

static int ignore_pattern_matches_core(const char *pattern,
                                       const char *filename, int rooted);

/* Match minimatch @(a|b) alternatives directly, trying each alternative as its
   own pattern. This avoids lowering '|' to brace-comma syntax, which would
   confuse a literal comma inside an alternative with an alternative split. */
static int ignore_extglob_alternative_matches(const char *pattern,
                                              const char *filename, int rooted,
                                              int *handled) {
  const char *cursor;
  *handled = 0;
  for (cursor = pattern; *cursor != '\0'; ++cursor) {
    const char *group_start;
    const char *body;
    const char *close = NULL;
    const char *scan;
    const char *alternative;
    size_t depth = 1;
    int in_bracket = 0;
    int negated_group = 0;
    size_t candidate_length;
    char *candidate;
    int candidate_matches;
    size_t prefix_length;
    size_t suffix_length;
    const char *suffix;
    if (*cursor == '\\' && cursor[1] != '\0') {
      ++cursor;
      continue;
    }
    if (*cursor == '[') {
      for (++cursor; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' && cursor[1] != '\0')
          ++cursor;
        else if (*cursor == ']')
          break;
      }
      if (*cursor == '\0')
        return 0;
      continue;
    }
    if (cursor[0] != '@' || cursor[1] != '(')
      continue;
    group_start = cursor;
    body = cursor + 2;
    if (*body == '!')
      negated_group = 1;
    for (scan = body; *scan != '\0'; ++scan) {
      if (*scan == '\\' && scan[1] != '\0') {
        ++scan;
      } else if (in_bracket) {
        if (*scan == ']')
          in_bracket = 0;
      } else if (*scan == '[') {
        in_bracket = 1;
      } else if (*scan == '(') {
        ++depth;
      } else if (*scan == ')') {
        if (--depth == 0) {
          close = scan;
          break;
        }
      }
    }
    if (close == NULL)
      continue;
    /* Only model '!(...)' when the group stands alone between literals in its
       path component. Adjacent wildcards let the surrounding glob absorb part
       of the name, which needs full component matching; defer those shapes to
       the platform matcher instead of inventing a subset. */
    if (negated_group) {
      char before = group_start == pattern ? '\0' : group_start[-1];
      const char *after = close + 1;
      if (before == '*' || before == '?' || before == '[' || *after == '*' ||
          *after == '?' || *after == '[')
        continue;
    }
    prefix_length = (size_t)(group_start - pattern);
    suffix = close + 1;
    suffix_length = strlen(suffix);
    if (negated_group) {
      /* '@(!(a|b))' matches only when the candidate matches none of the inner
         alternatives. Rebuild a positive '@(alt)' trial for each alternative
         and reject the whole group on the first hit. */
      const char *inner_open = body + 1;
      const char *inner_close = NULL;
      size_t inner_depth = 1;
      int inner_bracket = 0;
      for (scan = inner_open + 1; *scan != '\0'; ++scan) {
        if (*scan == '\\' && scan[1] != '\0') {
          ++scan;
        } else if (inner_bracket) {
          if (*scan == ']')
            inner_bracket = 0;
        } else if (*scan == '[') {
          inner_bracket = 1;
        } else if (*scan == '(') {
          ++inner_depth;
        } else if (*scan == ')') {
          if (--inner_depth == 0) {
            inner_close = scan;
            break;
          }
        }
      }
      if (inner_close == NULL)
        continue;
      *handled = 1;
      /* The group matches any segment, so model the whole pattern with the
         group replaced by '*'. If that baseline does not match, neither can
         the negation. Runs of '*' are squeezed afterwards so a group adjacent
         to a wildcard collapses instead of forming '**', which would wrongly
         acquire whole-path semantics. */
      candidate_length = prefix_length + 1 + suffix_length;
      candidate = malloc(candidate_length + 1);
      if (candidate == NULL)
        return -1;
      memcpy(candidate, pattern, prefix_length);
      candidate[prefix_length] = '*';
      memcpy(candidate + prefix_length + 1, suffix, suffix_length);
      candidate[candidate_length] = '\0';
      {
        size_t read = 0;
        size_t write = 0;
        while (read < candidate_length) {
          candidate[write++] = candidate[read];
          if (candidate[read] == '*') {
            while (read + 1 < candidate_length && candidate[read + 1] == '*')
              ++read;
          }
          ++read;
        }
        candidate[write] = '\0';
      }
      candidate_matches =
          ignore_pattern_matches_core(candidate, filename, rooted);
      free(candidate);
      if (!candidate_matches)
        return 0;
      depth = 1;
      in_bracket = 0;
      alternative = inner_open + 1;
      for (scan = inner_open + 1;; ++scan) {
        int delimiter = scan == inner_close;
        if (!delimiter) {
          if (*scan == '\\' && scan[1] != '\0') {
            ++scan;
          } else if (in_bracket) {
            if (*scan == ']')
              in_bracket = 0;
          } else if (*scan == '[') {
            in_bracket = 1;
          } else if (*scan == '(') {
            ++depth;
          } else if (*scan == ')') {
            --depth;
          } else if (*scan == '|' && depth == 1) {
            delimiter = 1;
          }
        }
        if (delimiter) {
          size_t alternative_length = (size_t)(scan - alternative);
          size_t trial_length =
              prefix_length + alternative_length + suffix_length;
          char *trial;
          int matches;
          if (prefix_length > SIZE_MAX - alternative_length ||
              prefix_length + alternative_length > SIZE_MAX - suffix_length)
            return -1;
          trial = malloc(trial_length + 1);
          if (trial == NULL)
            return -1;
          memcpy(trial, pattern, prefix_length);
          memcpy(trial + prefix_length, alternative, alternative_length);
          memcpy(trial + prefix_length + alternative_length, suffix,
                 suffix_length);
          trial[trial_length] = '\0';
          matches = ignore_pattern_matches_core(trial, filename, rooted);
          free(trial);
          if (matches)
            return 0;
          alternative = scan + 1;
        }
        if (scan == inner_close)
          break;
      }
      /* Every listed alternative failed, so the negation holds. */
      return 1;
    }
    *handled = 1;
    depth = 1;
    in_bracket = 0;
    alternative = body;
    for (scan = body;; ++scan) {
      int delimiter = scan == close;
      if (!delimiter) {
        if (*scan == '\\' && scan[1] != '\0') {
          ++scan;
        } else if (in_bracket) {
          if (*scan == ']')
            in_bracket = 0;
        } else if (*scan == '[') {
          in_bracket = 1;
        } else if (*scan == '(') {
          ++depth;
        } else if (*scan == ')') {
          --depth;
        } else if (*scan == '|' && depth == 1) {
          delimiter = 1;
        }
      }
      if (delimiter) {
        size_t alternative_length = (size_t)(scan - alternative);
        size_t trial_length;
        char *trial;
        int matches;
        if (prefix_length > SIZE_MAX - alternative_length ||
            prefix_length + alternative_length > SIZE_MAX - suffix_length)
          return -1;
        trial_length = prefix_length + alternative_length + suffix_length;
        trial = malloc(trial_length + 1);
        if (trial == NULL)
          return -1;
        memcpy(trial, pattern, prefix_length);
        memcpy(trial + prefix_length, alternative, alternative_length);
        memcpy(trial + prefix_length + alternative_length, suffix,
               suffix_length);
        trial[trial_length] = '\0';
        matches = ignore_pattern_matches_core(trial, filename, rooted);
        free(trial);
        if (matches)
          return 1;
        alternative = scan + 1;
      }
      if (scan == close)
        break;
    }
    return 0;
  }
  return 0;
}

static char *ignore_pattern_replace_brace(const char *pattern, const char *open,
                                          const char *close,
                                          const char *replacement,
                                          size_t replacement_length) {
  size_t prefix_length = (size_t)(open - pattern);
  size_t suffix_length = strlen(close + 1);
  size_t prefix_replacement_length;
  size_t expanded_length;
  char *expanded;
  if (prefix_length > SIZE_MAX - replacement_length)
    return NULL;
  prefix_replacement_length = prefix_length + replacement_length;
  if (prefix_replacement_length == SIZE_MAX ||
      suffix_length > SIZE_MAX - prefix_replacement_length - 1)
    return NULL;
  expanded_length = prefix_replacement_length + suffix_length;
  expanded = malloc(expanded_length + 1);
  if (expanded == NULL)
    return NULL;
  memcpy(expanded, pattern, prefix_length);
  memcpy(expanded + prefix_length, replacement, replacement_length);
  memcpy(expanded + prefix_replacement_length, close + 1, suffix_length);
  expanded[expanded_length] = '\0';
  return expanded;
}

static int parse_brace_integer(const char *text, long long *value) {
  const char *digit = text;
  char *end;
  long long parsed;
  if (*digit == '-')
    ++digit;
  if (*digit == '\0')
    return 0;
  for (; *digit != '\0'; ++digit) {
    if (!isdigit((unsigned char)*digit))
      return 0;
  }
  errno = 0;
  parsed = strtoll(text, &end, 10);
  if (errno == ERANGE || *end != '\0')
    return 0;
  *value = parsed;
  return 1;
}

static int ascii_letter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

static int ignore_brace_sequence_matches(const char *pattern, const char *open,
                                         const char *close,
                                         const char *filename, int rooted) {
  size_t body_length = (size_t)(close - open - 1);
  char *body = malloc(body_length + 1);
  char *separator;
  char *end_text;
  char *step_text = NULL;
  long long first, last, step = 1;
  int alpha;
  int padded = 0;
  size_t width;
  if (body == NULL)
    return 0;
  memcpy(body, open + 1, body_length);
  body[body_length] = '\0';
  separator = strstr(body, "..");
  if (separator == NULL) {
    free(body);
    return -1;
  }
  *separator = '\0';
  end_text = separator + 2;
  separator = strstr(end_text, "..");
  if (separator != NULL) {
    *separator = '\0';
    step_text = separator + 2;
    if (strstr(step_text, "..") != NULL ||
        !parse_brace_integer(step_text, &step)) {
      free(body);
      return -1;
    }
  }
  width = strlen(body) > strlen(end_text) ? strlen(body) : strlen(end_text);
  alpha = strlen(body) == 1 && strlen(end_text) == 1 && ascii_letter(body[0]) &&
          ascii_letter(end_text[0]);
  if (alpha) {
    first = (unsigned char)body[0];
    last = (unsigned char)end_text[0];
  } else if (!parse_brace_integer(body, &first) ||
             !parse_brace_integer(end_text, &last)) {
    free(body);
    return -1;
  }
  if (step_text == NULL)
    step = 1;
  if (step < 0)
    step = step == LLONG_MIN ? LLONG_MAX : -step;
  if (step == 0)
    step = 1;
  if (!alpha) {
    const char *first_digits = body[0] == '-' ? body + 1 : body;
    const char *last_digits = end_text[0] == '-' ? end_text + 1 : end_text;
    padded = (first_digits[0] == '0' && first_digits[1] != '\0') ||
             (last_digits[0] == '0' && last_digits[1] != '\0');
  }
  int ascending = first <= last;
  for (size_t count = 0; count < 100000; ++count) {
    char item[64];
    char numeric[64];
    size_t item_length;
    char *expanded;
    int numeric_length;
    if ((ascending && first > last) || (!ascending && first < last))
      break;
    if (alpha) {
      if (first == '\\') {
        item_length = 0;
      } else {
        item[0] = (char)first;
        item_length = 1;
      }
    } else {
      numeric_length = snprintf(numeric, sizeof numeric, "%lld", first);
      if (numeric_length < 0 || (size_t)numeric_length >= sizeof numeric) {
        free(body);
        return 0;
      }
      item_length = (size_t)numeric_length;
      if (padded && item_length < width) {
        size_t sign_length = numeric[0] == '-' ? 1 : 0;
        size_t zero_count = width - item_length;
        if (sign_length != 0)
          item[0] = '-';
        memset(item + sign_length, '0', zero_count);
        memcpy(item + sign_length + zero_count, numeric + sign_length,
               item_length - sign_length);
        item_length += zero_count;
      } else {
        memcpy(item, numeric, item_length);
      }
    }
    expanded =
        ignore_pattern_replace_brace(pattern, open, close, item, item_length);
    if (expanded == NULL) {
      free(body);
      return 0;
    }
    int matches = ignore_pattern_matches_core(expanded, filename, rooted);
    free(expanded);
    if (matches) {
      free(body);
      return 1;
    }
    if (first == last)
      break;
    if (ascending) {
      if (first > LLONG_MAX - step)
        break;
      first += step;
    } else {
      if (first < LLONG_MIN + step)
        break;
      first -= step;
    }
  }
  free(body);
  return 0;
}

static int ignore_pattern_matches_core(const char *pattern,
                                       const char *filename, int rooted) {
  int extglob_handled = 0;
  int extglob_match = ignore_extglob_alternative_matches(
      pattern, filename, rooted, &extglob_handled);
  if (extglob_match < 0)
    return 0;
  if (extglob_handled)
    return extglob_match;
  const char *open = NULL;
  for (const char *cursor = pattern; *cursor != '\0'; ++cursor) {
    if (*cursor == '\\' && cursor[1] != '\0') {
      ++cursor;
    } else if (*cursor == '{') {
      open = cursor;
      break;
    }
  }
  if (open != NULL) {
    const char *close = NULL;
    size_t depth = 1;
    int has_alternatives = 0;
    for (const char *cursor = open + 1; *cursor != '\0'; ++cursor) {
      if (*cursor == '\\' && cursor[1] != '\0') {
        ++cursor;
      } else if (*cursor == '{') {
        ++depth;
      } else if (*cursor == '}') {
        if (--depth == 0) {
          close = cursor;
          break;
        }
      } else if (*cursor == ',' && depth == 1) {
        has_alternatives = 1;
      }
    }
    if (close != NULL && has_alternatives) {
      size_t prefix_length = (size_t)(open - pattern);
      size_t suffix_length = strlen(close + 1);
      const char *alternative = open + 1;
      depth = 1;
      for (const char *cursor = alternative;; ++cursor) {
        int delimiter = cursor == close;
        if (!delimiter) {
          if (*cursor == '\\' && cursor[1] != '\0') {
            ++cursor;
          } else if (*cursor == '{') {
            ++depth;
          } else if (*cursor == '}') {
            --depth;
          } else if (*cursor == ',' && depth == 1) {
            delimiter = 1;
          }
        }
        if (delimiter) {
          size_t alternative_length = (size_t)(cursor - alternative);
          size_t prefix_alternative_length;
          size_t expanded_length;
          char *expanded;
          if (prefix_length > SIZE_MAX - alternative_length)
            return 0;
          prefix_alternative_length = prefix_length + alternative_length;
          if (prefix_alternative_length == SIZE_MAX ||
              suffix_length > SIZE_MAX - prefix_alternative_length - 1)
            return 0;
          expanded_length = prefix_alternative_length + suffix_length;
          expanded = malloc(expanded_length + 1);
          if (expanded == NULL)
            return 0;
          memcpy(expanded, pattern, prefix_length);
          memcpy(expanded + prefix_length, alternative, alternative_length);
          memcpy(expanded + prefix_alternative_length, close + 1,
                 suffix_length);
          expanded[expanded_length] = '\0';
          int matches = ignore_pattern_matches_core(expanded, filename, rooted);
          free(expanded);
          if (matches)
            return 1;
          alternative = cursor + 1;
        }
        if (cursor == close)
          break;
      }
      return 0;
    }
    if (close != NULL) {
      int sequence_match =
          ignore_brace_sequence_matches(pattern, open, close, filename, rooted);
      if (sequence_match >= 0)
        return sequence_match;
    }
  }
  if (rooted)
    return ignore_path_pattern_matches(pattern, filename);
  for (;;) {
    const char *slash = strchr(filename, '/');
    size_t length =
        slash == NULL ? strlen(filename) : (size_t)(slash - filename);
    char component[CSEMVER_PATH_MAX];
    if (length >= sizeof component)
      return 0;
    memcpy(component, filename, length);
    component[length] = '\0';
    if (fnmatch(pattern, component, IGNORE_FNM_FLAGS) == 0)
      return 1;
    if (slash == NULL)
      return 0;
    filename = slash + 1;
  }
}

static size_t minimatch_whitespace_length(const unsigned char *text,
                                          size_t length) {
  if (length == 0)
    return 0;
  if (text[0] == 0x09 || text[0] == 0x0a ||
      (text[0] >= 0x0b && text[0] <= 0x0d) || text[0] == 0x20)
    return 1;
  if (length >= 2 && text[0] == 0xc2 && text[1] == 0xa0)
    return 2;
  if (length >= 3 && text[0] == 0xe1 && text[1] == 0x9a && text[2] == 0x80)
    return 3;
  if (length >= 3 && text[0] == 0xe2 && text[1] == 0x80 &&
      ((text[2] >= 0x80 && text[2] <= 0x8a) || text[2] == 0xa8 ||
       text[2] == 0xa9 || text[2] == 0xaf))
    return 3;
  if (length >= 3 && text[0] == 0xe2 && text[1] == 0x81 && text[2] == 0x9f)
    return 3;
  if (length >= 3 && text[0] == 0xe3 && text[1] == 0x80 && text[2] == 0x80)
    return 3;
  if (length >= 3 && text[0] == 0xef && text[1] == 0xbb && text[2] == 0xbf)
    return 3;
  return 0;
}

static size_t utf8_character_length(const unsigned char *text, size_t length) {
  unsigned char first = text[0];
  if (first < 0x80)
    return 1;
  if (first >= 0xc2 && first <= 0xdf && length >= 2)
    return 2;
  if (first >= 0xe0 && first <= 0xef && length >= 3)
    return 3;
  if (first >= 0xf0 && first <= 0xf4 && length >= 4)
    return 4;
  return 1;
}

static int ignore_pattern_matches(const char *pattern, const char *filename,
                                  int rooted) {
  const unsigned char *text = (const unsigned char *)pattern;
  size_t length = strlen(pattern);
  size_t start = 0;
  size_t offset;
  size_t retained;
  size_t whitespace;
  size_t negations = 0;
  size_t match_start;
  char *trimmed;
  int matches;
  while ((whitespace =
              minimatch_whitespace_length(text + start, length - start)) != 0)
    start += whitespace;
  if (start == length || text[start] == '#')
    return 0;
  offset = retained = start;
  while (offset < length) {
    whitespace = minimatch_whitespace_length(text + offset, length - offset);
    if (whitespace != 0) {
      offset += whitespace;
      continue;
    }
    offset += utf8_character_length(text + offset, length - offset);
    retained = offset;
  }
  while (start + negations < retained && text[start + negations] == '!')
    ++negations;
  match_start = start + negations;
  if (match_start == retained)
    return 0;
  if (start == 0 && retained == length && negations == 0)
    return ignore_pattern_matches_core(pattern, filename, rooted);
  trimmed = malloc(retained - match_start + 1);
  if (trimmed == NULL)
    return 0;
  memcpy(trimmed, pattern + match_start, retained - match_start);
  trimmed[retained - match_start] = '\0';
  matches = ignore_pattern_matches_core(trimmed, filename, rooted);
  free(trimmed);
  return (negations & 1) != 0 ? !matches : matches;
}

static int nearest_gitignore_path(char path[CSEMVER_PATH_MAX]) {
  char directory[CSEMVER_PATH_MAX];
  if (getcwd(directory, sizeof directory) == NULL)
    return 0;
  for (;;) {
    int length = snprintf(path, CSEMVER_PATH_MAX, "%s%s.gitignore", directory,
                          strcmp(directory, "/") == 0 ? "" : "/");
    if (length >= 0 && length < CSEMVER_PATH_MAX && access(path, F_OK) == 0)
      return 1;
    if (strcmp(directory, "/") == 0)
      return 0;
    char *slash = strrchr(directory, '/');
    if (slash == NULL)
      return 0;
    if (slash == directory)
      directory[1] = '\0';
    else
      *slash = '\0';
  }
}

int csemver_path_is_gitignored(const char *filename) {
  char ignore_path[CSEMVER_PATH_MAX];
  char *contents = NULL;
  size_t contents_length = 0;
  size_t position = 0;
  int ignored = 0;
  if (!nearest_gitignore_path(ignore_path) ||
      !csemver_read_file(ignore_path, &contents, &contents_length))
    return 0;
  while (position < contents_length) {
    size_t start = position;
    size_t length;
    char *pattern;
    int negated = 0;
    int rooted = 0;
    while (position < contents_length && contents[position] != '\n' &&
           contents[position] != '\r')
      ++position;
    length = position - start;
    while (position < contents_length &&
           (contents[position] == '\n' || contents[position] == '\r'))
      ++position;
    pattern = malloc(length + 1);
    if (pattern == NULL)
      break;
    memcpy(pattern, contents + start, length);
    pattern[length] = '\0';
    if (pattern[0] == '!') {
      negated = 1;
      memmove(pattern, pattern + 1, strlen(pattern));
    }
    if (pattern[0] == '/') {
      rooted = 1;
      memmove(pattern, pattern + 1, strlen(pattern));
    }
    if (pattern[0] != '\0') {
      if (strchr(pattern, '/') != NULL)
        rooted = 1;
      if (ignore_pattern_matches(pattern, filename, rooted))
        ignored = !negated;
    }
    free(pattern);
  }
  free(contents);
  return ignored;
}
