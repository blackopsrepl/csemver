#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "changelog.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "common.h"
#include "config.h"
#include "semver.h"
#include "version.h"

static char *trim(char *text) {
  size_t length;
  while (*text != '\0' && isspace((unsigned char)*text))
    ++text;
  length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';
  return text;
}

int csemver_type_index(const CsemverConfig *config, const char *type) {
  size_t i;
  for (i = 0; i < config->commit_type_count; ++i)
    if (strcmp(config->commit_types[i].type, type) == 0)
      return (int)i;
  return -1;
}

static const CsemverCommitType angular_commit_types[] = {
    {"feat", "Features", false, true},
    {"fix", "Bug Fixes", false, false},
    {"perf", "Performance Improvements", false, false},
    {"revert", "Reverts", false, false},
    {"docs", "Documentation", true, false},
    {"style", "Styles", true, false},
    {"refactor", "Code Refactoring", true, false},
    {"test", "Tests", true, false},
    {"build", "Build System", true, false},
    {"ci", "Continuous Integration", true, false},
};

int csemver_preset_is_angular(const CsemverConfig *config) {
  return strcmp(config->preset, "angular") == 0 ||
         strcmp(config->preset, "conventional-changelog-angular") == 0;
}

int csemver_preset_is_supported(const CsemverConfig *config) {
  return csemver_preset_is_angular(config) ||
         strcmp(config->preset, "conventional-changelog-conventionalcommits") ==
             0;
}

int csemver_body_has_breaking_note(const char *body) {
  const char *line;
  if (body == NULL)
    return 0;
  line = body;
  while (*line != '\0') {
    while (*line == '\n' || *line == '\r' || *line == ' ' || *line == '\t')
      ++line;
    if (strncmp(line, "BREAKING CHANGE:", 16) == 0 ||
        strncmp(line, "BREAKING-CHANGE:", 16) == 0)
      return 1;
    line = strchr(line, '\n');
    if (line == NULL)
      break;
    ++line;
  }
  return 0;
}

int csemver_commit_is_breaking(const char *subject, const char *body) {
  const char *colon = strchr(subject, ':');
  if (colon != NULL && colon > subject && colon[-1] == '!')
    return 1;
  return csemver_body_has_breaking_note(body);
}

static int conventional_type(const char *subject, char *type, size_t size,
                             char *scope, size_t scope_size,
                             const char **description) {
  const char *colon = strchr(subject, ':');
  const char *open;
  const char *close;
  size_t type_size;
  if (colon == NULL)
    return 0;
  open = memchr(subject, '(', (size_t)(colon - subject));
  close = memchr(subject, ')', (size_t)(colon - subject));
  type_size =
      open == NULL ? (size_t)(colon - subject) : (size_t)(open - subject);
  if (type_size == 0 || type_size >= size)
    return 0;
  memcpy(type, subject, type_size);
  type[type_size] = '\0';
  if (open != NULL && close != NULL && close > open + 1 &&
      (size_t)(close - open - 1) < scope_size) {
    memcpy(scope, open + 1, (size_t)(close - open - 1));
    scope[close - open - 1] = '\0';
  } else
    scope[0] = '\0';
  if (open == NULL && colon > subject && colon[-1] == '!')
    type[type_size - 1] = '\0';
  *description = colon + 1;
  while (**description == ' ')
    ++*description;
  return 1;
}

static int angular_conventional_type(const char *subject, char *type,
                                     size_t size, char *scope,
                                     size_t scope_size,
                                     const char **description) {
  const char *colon = strchr(subject, ':');
  const char *open;
  const char *close = NULL;
  const char *cursor;
  size_t type_size, scope_length = 0;
  if (colon == NULL || colon[1] != ' ')
    return 0;
  open = memchr(subject, '(', (size_t)(colon - subject));
  type_size =
      open == NULL ? (size_t)(colon - subject) : (size_t)(open - subject);
  if (type_size == 0 || type_size >= size)
    return 0;
  for (cursor = subject; cursor < (open == NULL ? colon : open); ++cursor)
    if (!isalnum((unsigned char)*cursor) && *cursor != '_')
      return 0;
  if (open != NULL) {
    for (cursor = colon; cursor > open; --cursor)
      if (cursor[-1] == ')') {
        close = cursor - 1;
        break;
      }
    if (close == NULL || close != colon - 1)
      return 0;
    scope_length = (size_t)(close - open - 1);
    if (scope_length >= scope_size)
      return 0;
    memcpy(scope, open + 1, scope_length);
  }
  memcpy(type, subject, type_size);
  type[type_size] = '\0';
  scope[scope_length] = '\0';
  *description = colon + 2;
  return 1;
}

int csemver_preset_commit_type(const CsemverConfig *config, const char *subject,
                              char *type, size_t size, char *scope,
                              size_t scope_size, const char **description) {
  if (csemver_preset_is_angular(config))
    return angular_conventional_type(subject, type, size, scope, scope_size,
                                     description);
  return conventional_type(subject, type, size, scope, scope_size, description);
}

typedef struct {
  char host[CSEMVER_PATH_MAX];
  char owner[CSEMVER_PATH_MAX];
  char repository[CSEMVER_PATH_MAX];
  const char *previous_tag;
  const char *current_tag;
  const char *hash;
  const char *id;
  const char *prefix;
  const char *user;
} CsemverUrlContext;

static void url_context_init(CsemverUrlContext *context, const char *base,
                             const char *previous_tag, const char *current_tag,
                             const char *hash, const char *id,
                             const char *prefix, const char *user) {
  const char *authority, *path, *path_end, *last_slash = NULL, *cursor;
  const char *scheme;
  memset(context, 0, sizeof *context);
  context->previous_tag = previous_tag == NULL ? "" : previous_tag;
  context->current_tag = current_tag == NULL ? "" : current_tag;
  context->hash = hash == NULL ? "" : hash;
  context->id = id == NULL ? "" : id;
  context->prefix = prefix == NULL ? "" : prefix;
  context->user = user == NULL ? "" : user;
  if (base == NULL || base[0] == '\0')
    return;
  scheme = strstr(base, "://");
  authority = scheme == NULL ? base : scheme + 3;
  path = strchr(authority, '/');
  if (path == NULL)
    return;
  snprintf(context->host, sizeof context->host, "%.*s", (int)(path - base),
           base);
  ++path;
  path_end = path + strlen(path);
  while (path_end > path && path_end[-1] == '/')
    --path_end;
  for (cursor = path; cursor < path_end; ++cursor)
    if (*cursor == '/')
      last_slash = cursor;
  if (last_slash == NULL) {
    snprintf(context->repository, sizeof context->repository, "%.*s",
             (int)(path_end - path), path);
  } else {
    snprintf(context->owner, sizeof context->owner, "%.*s",
             (int)(last_slash - path), path);
    snprintf(context->repository, sizeof context->repository, "%.*s",
             (int)(path_end - last_slash - 1), last_slash + 1);
  }
}

static const char *url_template_value(const CsemverUrlContext *context,
                                      const char *name, size_t name_length) {
  if (name_length == 4 && memcmp(name, "host", 4) == 0)
    return context->host;
  if (name_length == 5 && memcmp(name, "owner", 5) == 0)
    return context->owner;
  if (name_length == 10 && memcmp(name, "repository", 10) == 0)
    return context->repository;
  if (name_length == 11 && memcmp(name, "previousTag", 11) == 0)
    return context->previous_tag;
  if (name_length == 10 && memcmp(name, "currentTag", 10) == 0)
    return context->current_tag;
  if (name_length == 4 && memcmp(name, "hash", 4) == 0)
    return context->hash;
  if (name_length == 2 && memcmp(name, "id", 2) == 0)
    return context->id;
  if (name_length == 6 && memcmp(name, "prefix", 6) == 0)
    return context->prefix;
  if (name_length == 4 && memcmp(name, "user", 4) == 0)
    return context->user;
  return "";
}

static int append_url_template(CsemverBuffer *output, const char *format,
                               const CsemverUrlContext *context) {
  const char *literal = format;
  const char *cursor = format;
  while (*cursor != '\0') {
    if (cursor[0] == '{' && cursor[1] == '{') {
      const char *close = strstr(cursor + 2, "}}");
      if (close != NULL) {
        const char *name = cursor + 2;
        const char *name_end = close;
        const char *value;
        while (name < name_end && isspace((unsigned char)*name))
          ++name;
        while (name_end > name && isspace((unsigned char)name_end[-1]))
          --name_end;
        if (!csemver_buffer_append(output, literal, (size_t)(cursor - literal)))
          return 0;
        value = url_template_value(context, name, (size_t)(name_end - name));
        if (!csemver_buffer_append(output, value, strlen(value)))
          return 0;
        cursor = close + 2;
        literal = cursor;
        continue;
      }
    }
    ++cursor;
  }
  return csemver_buffer_append(output, literal, (size_t)(cursor - literal));
}

static const char *effective_url_format(const char *format,
                                        const char *default_format) {
  return format[0] == '\0' ? default_format : format;
}

static int url_format_is_enabled(const char *base, bool explicit_format,
                                 const char *format) {
  return (base != NULL && base[0] != '\0') ||
         (explicit_format && format[0] != '\0');
}

static int append_issue_link(CsemverBuffer *out, const CsemverConfig *config,
                             const char *text, const char *base) {
  static const char default_issue_format[] =
      "{{host}}/{{owner}}/{{repository}}/issues/{{id}}";
  static const char default_user_format[] = "{{host}}/{{user}}";
  const char *issue_format =
      effective_url_format(config->issue_url_format, default_issue_format);
  const char *user_format =
      effective_url_format(config->user_url_format, default_user_format);
  size_t i;
  for (i = 0; text[i] != '\0';) {
    size_t prefix;
    bool matched = false;
    for (prefix = 0; prefix < config->issue_prefix_count; ++prefix) {
      const char *token = config->issue_prefixes[prefix];
      size_t token_len = strlen(token);
      size_t digits = 0;
      if (token_len == 0 || strncmp(text + i, token, token_len) != 0)
        continue;
      while (text[i + token_len + digits] != '\0' &&
             isdigit((unsigned char)text[i + token_len + digits]))
        ++digits;
      if (digits > 0 &&
          url_format_is_enabled(base, config->issue_url_format_explicit,
                                config->issue_url_format)) {
        char id[32];
        CsemverUrlContext context;
        size_t id_len = token_len + digits;
        if (id_len >= sizeof id)
          return 0;
        memcpy(id, text + i + token_len, digits);
        id[digits] = '\0';
        url_context_init(&context, base, NULL, NULL, NULL, id, token, NULL);
        if (!csemver_buffer_appendf(out, "[%.*s](", (int)id_len, text + i) ||
            !append_url_template(out, issue_format, &context) ||
            !csemver_buffer_append(out, ")", 1))
          return 0;
        i += id_len;
        matched = true;
        break;
      }
    }
    if (matched)
      continue;
    if (text[i] == '@' &&
        (i == 0 || (!isalnum((unsigned char)text[i - 1]) &&
                    text[i - 1] != '_' && text[i - 1] != '-'))) {
      char user[128];
      size_t end = i + 1, user_length;
      while (isalnum((unsigned char)text[end]) || text[end] == '_' ||
             text[end] == '-')
        ++end;
      user_length = end - i - 1;
      if (user_length > 0 && user_length < sizeof user &&
          url_format_is_enabled(base, config->user_url_format_explicit,
                                config->user_url_format)) {
        CsemverUrlContext context;
        memcpy(user, text + i + 1, user_length);
        user[user_length] = '\0';
        url_context_init(&context, base, NULL, NULL, NULL, NULL, NULL, user);
        if (!csemver_buffer_append(out, "[@", 2) ||
            !csemver_buffer_append(out, user, user_length) ||
            !csemver_buffer_append(out, "](", 2) ||
            !append_url_template(out, user_format, &context) ||
            !csemver_buffer_append(out, ")", 1))
          return 0;
        i = end;
        continue;
      }
    }
    if (!csemver_buffer_append(out, text + i, 1))
      return 0;
    ++i;
  }
  return 1;
}

static int ascii_equal_fold(char left, char right) {
  return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static int text_contains_reference(const char *text, const char *reference) {
  size_t i, reference_length = strlen(reference);
  for (i = 0; text[i] != '\0'; ++i) {
    size_t j = 0;
    while (j < reference_length && text[i + j] != '\0' &&
           ascii_equal_fold(text[i + j], reference[j]))
      ++j;
    if (j == reference_length &&
        !isalnum((unsigned char)text[i + reference_length]) &&
        text[i + reference_length] != '_' && text[i + reference_length] != '-')
      return 1;
  }
  return 0;
}

static int angular_revert_target(const Commit *commit, char target[41]) {
  static const char marker[] = "This reverts commit ";
  const char *cursor;
  if (commit == NULL || strlen(commit->subject) < 7 ||
      !ascii_equal_fold(commit->subject[0], 'r') ||
      !ascii_equal_fold(commit->subject[1], 'e') ||
      !ascii_equal_fold(commit->subject[2], 'v') ||
      !ascii_equal_fold(commit->subject[3], 'e') ||
      !ascii_equal_fold(commit->subject[4], 'r') ||
      !ascii_equal_fold(commit->subject[5], 't') ||
      (commit->subject[6] != ':' &&
       !isspace((unsigned char)commit->subject[6])) ||
      (commit->subject[6] == ':' &&
       !isspace((unsigned char)commit->subject[7])))
    return 0;
  for (cursor = commit->body; *cursor != '\0'; ++cursor) {
    size_t i;
    size_t target_length = 0;
    for (i = 0; i < sizeof marker - 1 && cursor[i] != '\0'; ++i)
      if (!ascii_equal_fold(cursor[i], marker[i]))
        break;
    if (i != sizeof marker - 1)
      continue;
    cursor += sizeof marker - 1;
    while (isspace((unsigned char)*cursor))
      ++cursor;
    while (isxdigit((unsigned char)*cursor)) {
      if (target_length == 40)
        break;
      target[target_length++] = *cursor++;
    }
    if (target_length < 7 || isalnum((unsigned char)*cursor) || *cursor == '_')
      continue;
    target[target_length] = '\0';
    return 1;
  }
  return 0;
}

static void collect_angular_revert_pairs(const Commit *commits,
                                         size_t commit_count,
                                         bool reverted[CSEMVER_COMMIT_MAX]) {
  size_t i;
  memset(reverted, 0, commit_count * sizeof *reverted);
  for (i = 0; i < commit_count; ++i) {
    char target[41];
    size_t target_length, j, match = 0, match_count = 0;
    if (!angular_revert_target(&commits[i], target))
      continue;
    target_length = strlen(target);
    for (j = 0; j < commit_count; ++j) {
      size_t k;
      if (j == i || strlen(commits[j].hash) < target_length)
        continue;
      for (k = 0; k < target_length; ++k)
        if (!ascii_equal_fold(commits[j].hash[k], target[k]))
          break;
      if (k == target_length) {
        match = j;
        ++match_count;
      }
    }
    if (match_count == 1) {
      reverted[i] = true;
      reverted[match] = true;
    }
  }
}

static int preset_commit_type_for_commit(const CsemverConfig *config,
                                         const Commit *commit, char *type,
                                         size_t size, char *scope,
                                         size_t scope_size,
                                         const char **description) {
  char target[41];
  if (csemver_preset_commit_type(config, commit->subject, type, size, scope, scope_size,
                         description))
    return 1;
  if (!csemver_preset_is_angular(config) || !angular_revert_target(commit, target) ||
      sizeof "revert" > size || scope_size == 0)
    return 0;
  memcpy(type, "revert", sizeof "revert");
  scope[0] = '\0';
  *description = commit->subject;
  return 1;
}

static int is_reference_word_char(char value) {
  return isalnum((unsigned char)value) || value == '_' || value == '-';
}

static int line_has_closing_action(const char *line, const char *end) {
  static const char *const actions[] = {"close",   "closes",   "closed",
                                        "fix",     "fixes",    "fixed",
                                        "resolve", "resolves", "resolved"};
  const char *cursor;
  for (cursor = line; cursor < end; ++cursor) {
    size_t action;
    for (action = 0; action < sizeof actions / sizeof actions[0]; ++action) {
      size_t length = strlen(actions[action]);
      size_t i;
      if ((size_t)(end - cursor) < length ||
          (cursor > line && is_reference_word_char(cursor[-1])) ||
          (cursor + length < end && is_reference_word_char(cursor[length])))
        continue;
      for (i = 0; i < length; ++i)
        if (!ascii_equal_fold(cursor[i], actions[action][i]))
          break;
      if (i == length)
        return 1;
    }
  }
  return 0;
}

static int reference_is_duplicate(const IssueReference *references,
                                  size_t reference_count, const char *text) {
  size_t i;
  for (i = 0; i < reference_count; ++i)
    if (strcmp(references[i].text, text) == 0)
      return 1;
  return 0;
}

static int collect_body_references(const CsemverConfig *config,
                                   const Commit *commit,
                                   IssueReference *references,
                                   size_t *reference_count) {
  const char *line = commit->body;
  *reference_count = 0;
  while (*line != '\0') {
    const char *line_end = strchr(line, '\n');
    const char *cursor;
    if (line_end == NULL)
      line_end = line + strlen(line);
    for (cursor = line; cursor < line_end;) {
      size_t prefix;
      int matched = 0;
      for (prefix = 0; prefix < config->issue_prefix_count; ++prefix) {
        const char *issue_start;
        const char *issue_end;
        size_t token_length;
        char token[CSEMVER_ISSUE_REFERENCE_TEXT_MAX];
        IssueReference *reference;
        if (config->issue_prefixes[prefix][0] == '\0' ||
            strncmp(cursor, config->issue_prefixes[prefix],
                    strlen(config->issue_prefixes[prefix])) != 0)
          continue;
        issue_start = cursor + strlen(config->issue_prefixes[prefix]);
        issue_end = issue_start;
        while (issue_end < line_end && is_reference_word_char(*issue_end))
          ++issue_end;
        if (issue_end == issue_start ||
            (issue_end < line_end && *issue_end != ' ' && *issue_end != '\t' &&
             *issue_end != ',' && *issue_end != ';' && *issue_end != '.' &&
             *issue_end != ')' && *issue_end != ']'))
          continue;
        token_length = (size_t)(issue_end - cursor);
        if (token_length >= sizeof token)
          return 0;
        memcpy(token, cursor, token_length);
        token[token_length] = '\0';
        if (!text_contains_reference(commit->subject, token) &&
            !reference_is_duplicate(references, *reference_count, token)) {
          if (*reference_count >= CSEMVER_ISSUE_REFERENCE_MAX)
            return 0;
          reference = &references[(*reference_count)++];
          memcpy(reference->text, token, token_length + 1);
          reference->closing = line_has_closing_action(line, cursor);
        }
        cursor = issue_end;
        matched = 1;
        break;
      }
      if (!matched)
        ++cursor;
    }
    line = *line_end == '\0' ? line_end : line_end + 1;
  }
  return 1;
}

static int append_body_references(CsemverBuffer *section,
                                  const CsemverConfig *config,
                                  const Commit *commit, const char *base) {
  IssueReference references[CSEMVER_ISSUE_REFERENCE_MAX];
  size_t reference_count, i;
  bool wrote;
  int closing;
  if (!collect_body_references(config, commit, references, &reference_count))
    return 0;
  for (closing = 1; closing >= 0; --closing) {
    wrote = false;
    for (i = 0; i < reference_count; ++i) {
      if (references[i].closing != (closing != 0))
        continue;
      if (!wrote) {
        if (!csemver_buffer_append(section,
                                   closing ? ", closes " : ", references ",
                                   closing ? 9 : 13))
          return 0;
        wrote = true;
      } else if (!csemver_buffer_append(section, " ", 1))
        return 0;
      if (!append_issue_link(section, config, references[i].text, base))
        return 0;
    }
  }
  return 1;
}

static void repository_base(char *base, size_t base_size) {
  const char *args[] = {"config", "--get", "remote.origin.url", NULL};
  char *output = NULL;
  char package_url[1024];
  char *package = NULL;
  char *url = NULL;
  char *path;
  int status = 0;
  base[0] = '\0';
  if (csemver_read_file("package.json", &package, NULL)) {
    if (csemver_json_repository_url(package, package_url, sizeof package_url))
      url = package_url;
    free(package);
  }
  if (url == NULL) {
    if (!csemver_run_git(args, &output, &status) || status != 0 || output == NULL) {
      free(output);
      return;
    }
    url = trim(output);
  }
  if (strncmp(url, "git@", 4) == 0) {
    char *colon = strchr(url, ':');
    if (colon != NULL) {
      *colon = '\0';
      path = colon + 1;
      snprintf(base, base_size, "https://%s/%s", url + 4, path);
    }
  } else {
    char *scheme = strstr(url, "://");
    if (scheme != NULL) {
      path = strchr(scheme + 3, '/');
      if (path != NULL) {
        *path++ = '\0';
        snprintf(base, base_size, "https://%s/%s", scheme + 3, path);
      }
    }
  }
  if (strlen(base) > 4 && strcmp(base + strlen(base) - 4, ".git") == 0)
    base[strlen(base) - 4] = '\0';
  free(output);
}

static int append_commit_line(CsemverBuffer *section,
                              const CsemverConfig *config, const Commit *commit,
                              const char *base) {
  char type[128], scope[256];
  const char *description;
  char short_hash[8];
  size_t i;
  if (!preset_commit_type_for_commit(config, commit, type, sizeof type, scope,
                                     sizeof scope, &description)) {
    description = commit->subject;
    type[0] = scope[0] = '\0';
  }
  for (i = 0; i < 7 && commit->hash[i] != '\0'; ++i)
    short_hash[i] = commit->hash[i];
  short_hash[i] = '\0';
  if (!csemver_buffer_append(section, "* ", 2))
    return 0;
  if (scope[0] != '\0' && !csemver_buffer_appendf(section, "**%s:** ", scope))
    return 0;
  if (!append_issue_link(section, config, description, base))
    return 0;
  if (url_format_is_enabled(base, config->commit_url_format_explicit,
                            config->commit_url_format)) {
    static const char default_commit_format[] =
        "{{host}}/{{owner}}/{{repository}}/commit/{{hash}}";
    CsemverUrlContext context;
    url_context_init(&context, base, NULL, NULL, commit->hash, NULL, NULL,
                     NULL);
    if (!csemver_buffer_appendf(section, " ([%s](", short_hash) ||
        !append_url_template(section,
                             effective_url_format(config->commit_url_format,
                                                  default_commit_format),
                             &context) ||
        !csemver_buffer_append(section, "))", 2))
      return 0;
  } else if (!csemver_buffer_appendf(section, " %s", short_hash))
    return 0;
  if (!append_body_references(section, config, commit, base))
    return 0;
  return csemver_buffer_append(section, "\n", 1);
}

static int append_sorted_group(CsemverBuffer *section,
                               const CsemverConfig *config,
                               const CommitSortKey *keys, const Commit *commits,
                               size_t commit_count, int type_index,
                               const char *base) {
  size_t order[CSEMVER_COMMIT_MAX];
  size_t count = 0, i;
  if (commit_count > CSEMVER_COMMIT_MAX)
    return 0;
  for (i = 0; i < commit_count; ++i)
    if (keys[i].type_index == type_index)
      order[count++] = i;
  for (i = 1; i < count; ++i) {
    size_t commit_index = order[i];
    size_t j = i;
    const char *key =
        keys[commit_index].key.data == NULL ? "" : keys[commit_index].key.data;
    while (j > 0) {
      const char *previous = keys[order[j - 1]].key.data == NULL
                                 ? ""
                                 : keys[order[j - 1]].key.data;
      if (strcmp(previous, key) <= 0)
        break;
      order[j] = order[j - 1];
      --j;
    }
    order[j] = commit_index;
  }
  for (i = 0; i < count; ++i)
    if (!append_commit_line(section, config, &commits[order[i]], base))
      return 0;
  return 1;
}

static int add_breaking_note(BreakingNote **notes, size_t *note_count,
                             size_t *note_capacity, const Commit *commit,
                             const char *text, size_t text_length) {
  BreakingNote *grown;
  size_t capacity;
  if (*note_count == *note_capacity) {
    capacity = *note_capacity == 0 ? 8 : *note_capacity * 2;
    if (capacity < *note_capacity || capacity > SIZE_MAX / sizeof **notes)
      return 0;
    grown = realloc(*notes, capacity * sizeof **notes);
    if (grown == NULL)
      return 0;
    *notes = grown;
    *note_capacity = capacity;
  }
  (*notes)[*note_count].commit = commit;
  (*notes)[*note_count].text = text;
  (*notes)[*note_count].text_length = text_length;
  ++*note_count;
  return 1;
}

static int append_breaking_note(CsemverBuffer *notes,
                                const CsemverConfig *config,
                                const BreakingNote *note, const char *base) {
  char type[128], scope[256];
  char *text;
  const char *description;
  int appended;
  if (!preset_commit_type_for_commit(config, note->commit, type, sizeof type,
                                     scope, sizeof scope, &description))
    scope[0] = '\0';
  text = malloc(note->text_length + 1);
  if (text == NULL)
    return 0;
  memcpy(text, note->text, note->text_length);
  text[note->text_length] = '\0';
  appended =
      csemver_buffer_append(notes, "* ", 2) &&
      (scope[0] == '\0' || csemver_buffer_appendf(notes, "**%s:** ", scope)) &&
      append_issue_link(notes, config, text, base) &&
      csemver_buffer_append(notes, "\n", 1);
  free(text);
  return appended;
}

static int collect_breaking_notes(const Commit *commit, BreakingNote **notes,
                                  size_t *note_count, size_t *note_capacity) {
  const char *line = commit->body;
  while (*line != '\0') {
    const char *start = line;
    const char *note = NULL;
    const char *end;
    while (*start == ' ' || *start == '\t')
      ++start;
    if (strncmp(start, "BREAKING CHANGE:", 16) == 0)
      note = start + 16;
    else if (strncmp(start, "BREAKING-CHANGE:", 16) == 0)
      note = start + 16;
    if (note != NULL) {
      while (*note == ' ' || *note == '\t')
        ++note;
      end = strchr(note, '\n');
      if (end == NULL)
        end = note + strlen(note);
      while (end > note && (end[-1] == ' ' || end[-1] == '\r'))
        --end;
      if (end > note && !add_breaking_note(notes, note_count, note_capacity,
                                           commit, note, (size_t)(end - note)))
        return 0;
    }
    line = strchr(line, '\n');
    if (line == NULL)
      break;
    ++line;
  }
  return 1;
}

static int compare_breaking_notes(const BreakingNote *left,
                                  const BreakingNote *right) {
  size_t common_length = left->text_length < right->text_length
                             ? left->text_length
                             : right->text_length;
  int order = memcmp(left->text, right->text, common_length);
  if (order != 0)
    return order;
  return left->text_length < right->text_length   ? -1
         : left->text_length > right->text_length ? 1
                                                  : 0;
}

/* Upstream sorts group titles (and tags) with the writer's case-insensitive
 * comparator, which places untitled groups first. */
static int case_insensitive_compare(const char *left, const char *right) {
  const unsigned char *a = (const unsigned char *)left;
  const unsigned char *b = (const unsigned char *)right;
  while (*a != '\0' && *b != '\0') {
    int lower_a = tolower(*a);
    int lower_b = tolower(*b);
    if (lower_a != lower_b)
      return lower_a < lower_b ? -1 : 1;
    ++a;
    ++b;
  }
  if (*a == *b)
    return 0;
  return *a == '\0' ? -1 : 1;
}

static int compare_group_titles(const CsemverConfig *config, size_t left,
                                size_t right) {
  return case_insensitive_compare(config->commit_types[left].section,
                                  config->commit_types[right].section);
}

int csemver_changelog_section(const CsemverConfig *config, const Commit *commits,
                             size_t commit_count, CsemverBuffer *output) {
  CsemverBuffer groups[CSEMVER_MAX_TYPES];
  CsemverBuffer breaking;
  CommitSortKey sort_keys[CSEMVER_COMMIT_MAX];
  BreakingNote *breaking_notes = NULL;
  size_t breaking_note_count = 0, breaking_note_capacity = 0;
  bool used[CSEMVER_MAX_TYPES] = {false};
  bool reverted[CSEMVER_COMMIT_MAX] = {false};
  char base[1024];
  size_t i, group_count;
  size_t group_order[CSEMVER_MAX_TYPES];
  bool angular = csemver_preset_is_angular(config);
  CsemverConfig angular_config;
  if (commit_count > CSEMVER_COMMIT_MAX)
    return 0;
  if (angular)
    collect_angular_revert_pairs(commits, commit_count, reverted);
  if (angular) {
    size_t preset_count =
        sizeof angular_commit_types / sizeof angular_commit_types[0];
    angular_config = *config;
    memset(angular_config.commit_types, 0, sizeof angular_config.commit_types);
    memcpy(angular_config.commit_types, angular_commit_types,
           sizeof angular_commit_types);
    angular_config.commit_type_count = preset_count;
    config = &angular_config;
    for (i = 0; i < commit_count; ++i) {
      char type[128], scope[256];
      const char *description;
      CsemverCommitType *entry;
      if (reverted[i] ||
          !preset_commit_type_for_commit(config, &commits[i], type, sizeof type,
                                         scope, sizeof scope, &description) ||
          !csemver_body_has_breaking_note(commits[i].body) ||
          strlen(type) >= sizeof angular_config.commit_types[0].type ||
          csemver_type_index(config, type) >= 0 ||
          angular_config.commit_type_count >= CSEMVER_MAX_TYPES)
        continue;
      entry = &angular_config.commit_types[angular_config.commit_type_count++];
      memcpy(entry->type, type, strlen(type) + 1);
      snprintf(entry->section, sizeof entry->section, "%s", type);
    }
  }
  group_count = config->commit_type_count;
  csemver_buffer_init(&breaking);
  repository_base(base, sizeof base);
  for (i = 0; i < group_count; ++i)
    csemver_buffer_init(&groups[i]);
  for (i = 0; i < commit_count; ++i) {
    sort_keys[i].type_index = -1;
    csemver_buffer_init(&sort_keys[i].key);
  }
  for (i = 0; i < commit_count; ++i) {
    char type[128], scope[256];
    const char *description;
    int index;
    int parsed;
    bool has_breaking_note;
    if (angular && reverted[i])
      continue;
    parsed =
        preset_commit_type_for_commit(config, &commits[i], type, sizeof type,
                                      scope, sizeof scope, &description);
    has_breaking_note = csemver_body_has_breaking_note(commits[i].body);
    if (!parsed)
      scope[0] = '\0';
    if (!collect_breaking_notes(&commits[i], &breaking_notes,
                                &breaking_note_count, &breaking_note_capacity))
      goto fail;
    if (parsed && !has_breaking_note &&
        csemver_commit_is_breaking(commits[i].subject, "") &&
        !add_breaking_note(&breaking_notes, &breaking_note_count,
                           &breaking_note_capacity, &commits[i], description,
                           strlen(description)))
      goto fail;
    if (!parsed)
      continue;
    index = csemver_type_index(config, type);
    if (index < 0 ||
        (config->commit_types[index].hidden && !has_breaking_note))
      continue;
    used[index] = true;
    sort_keys[i].type_index = index;
    if (strcmp(scope, "*") == 0)
      scope[0] = '\0';
    if (!csemver_buffer_append(&sort_keys[i].key, scope, strlen(scope)) ||
        !append_issue_link(&sort_keys[i].key, config, description, base))
      goto fail;
  }
  for (i = 1; i < breaking_note_count; ++i) {
    BreakingNote note = breaking_notes[i];
    size_t j = i;
    while (j > 0 && compare_breaking_notes(&note, &breaking_notes[j - 1]) < 0) {
      breaking_notes[j] = breaking_notes[j - 1];
      --j;
    }
    breaking_notes[j] = note;
  }
  for (i = 0; i < group_count; ++i)
    group_order[i] = i;
  if (angular) {
    /* Upstream sorts commit groups by title with the writer's
     * case-insensitive comparator; untitled groups (only possible when a
     * hidden type is forced visible by a breaking note) sort first. */
    for (i = 1; i < group_count; ++i) {
      size_t group_index = group_order[i];
      size_t j = i;
      while (j > 0 && compare_group_titles(config, group_order[j - 1],
                                           group_index) > 0) {
        group_order[j] = group_order[j - 1];
        --j;
      }
      group_order[j] = group_index;
    }
  } else {
    /* The conventionalcommits preset groups by type in configuration order,
     * but an untitled group (a hidden type kept visible by a breaking note)
     * is emitted before the titled groups. */
    size_t next = 0;
    for (i = 0; i < group_count; ++i)
      if (config->commit_types[i].section[0] == '\0')
        group_order[next++] = i;
    for (i = 0; i < group_count; ++i)
      if (config->commit_types[i].section[0] != '\0')
        group_order[next++] = i;
  }
  for (i = 0; i < breaking_note_count; ++i)
    if (!append_breaking_note(&breaking, config, &breaking_notes[i], base))
      goto fail;
  for (i = 0; i < group_count; ++i)
    if (used[i] && !append_sorted_group(&groups[i], config, sort_keys, commits,
                                        commit_count, (int)i, base))
      goto fail;
  if (!angular && breaking.length > 0) {
    if (!csemver_buffer_append(output, "\n### ⚠ BREAKING CHANGES\n\n",
                               sizeof "\n### ⚠ BREAKING CHANGES\n\n" - 1) ||
        !csemver_buffer_append(output, breaking.data, breaking.length))
      goto fail;
  }
  {
    for (i = 0; i < group_count; ++i) {
      size_t index = group_order[i];
      if (!used[index])
        continue;
      if (!csemver_buffer_append(output, "\n", 1))
        goto fail;
      if (config->commit_types[index].section[0] != '\0' &&
          !csemver_buffer_appendf(output, "### %s\n\n",
                                  config->commit_types[index].section))
        goto fail;
      if (!csemver_buffer_append(output, groups[index].data,
                                 groups[index].length) ||
          !csemver_buffer_append(output, "\n", 1))
        goto fail;
    }
    if (angular && breaking.length > 0) {
      if (!csemver_buffer_append(output, "\n### BREAKING CHANGES\n\n",
                                 sizeof "\n### BREAKING CHANGES\n\n" - 1) ||
          !csemver_buffer_append(output, breaking.data, breaking.length) ||
          !csemver_buffer_append(output, "\n", 1))
        goto fail;
    }
  }
  for (i = 0; i < group_count; ++i)
    csemver_buffer_free(&groups[i]);
  csemver_buffer_free(&breaking);
  for (i = 0; i < commit_count; ++i)
    csemver_buffer_free(&sort_keys[i].key);
  free(breaking_notes);
  return 1;
fail:
  for (i = 0; i < group_count; ++i)
    csemver_buffer_free(&groups[i]);
  csemver_buffer_free(&breaking);
  for (i = 0; i < commit_count; ++i)
    csemver_buffer_free(&sort_keys[i].key);
  free(breaking_notes);
  return 0;
}


static int stable_tag_version(const CsemverConfig *config, const char *tag,
                              char *version, size_t version_size) {
  size_t prefix_size = strlen(config->tag_prefix);
  Semver parsed;
  if (strncmp(tag, config->tag_prefix, prefix_size) != 0 ||
      strlen(tag + prefix_size) >= version_size ||
      !semver_parse(tag + prefix_size, &parsed) || parsed.has_prerelease)
    return 0;
  snprintf(version, version_size, "%s", tag + prefix_size);
  return 1;
}

static int get_tag_date(const char *tag, char *date, size_t date_size) {
  const char *args[] = {"log", "-1", "--format=%cs", tag, NULL};
  char *output = NULL;
  int status = 0;
  if (!csemver_run_git(args, &output, &status) || status != 0 || output == NULL) {
    free(output);
    return 0;
  }
  if (snprintf(date, date_size, "%s", trim(output)) >= (int)date_size) {
    free(output);
    return 0;
  }
  free(output);
  return 1;
}

static const char *release_heading_level(const CsemverConfig *config,
                                         const char *version) {
  Semver parsed;
  if (!csemver_preset_is_angular(config))
    return "##";
  if (!semver_parse(version, &parsed) || parsed.patch == 0)
    return "#";
  return "##";
}

static int append_compare_heading(const CsemverConfig *config,
                                  CsemverBuffer *output, const char *base,
                                  const char *version, const char *previous_tag,
                                  const char *tag, const char *date) {
  static const char default_compare_format[] =
      "{{host}}/{{owner}}/{{repository}}/compare/"
      "{{previousTag}}...{{currentTag}}";
  const char *level = release_heading_level(config, version);
  if (url_format_is_enabled(base, config->compare_url_format_explicit,
                            config->compare_url_format)) {
    CsemverUrlContext context;
    url_context_init(&context, base, previous_tag, tag, NULL, NULL, NULL, NULL);
    return csemver_buffer_appendf(output, "%s [%s](", level, version) &&
           append_url_template(output,
                               effective_url_format(config->compare_url_format,
                                                    default_compare_format),
                               &context) &&
           csemver_buffer_appendf(output, ") (%s)\n\n", date);
  }
  if (csemver_preset_is_angular(config))
    return csemver_buffer_appendf(output, "%s %s (%s)\n\n", level, version,
                                  date);
  return csemver_buffer_appendf(output, "## [%s](///compare/%s...%s) (%s)\n\n",
                                version, previous_tag, tag, date);
}

int csemver_append_release_heading(const CsemverConfig *config,
                                  CsemverBuffer *output, const char *base,
                                  const char *version, const char *previous_tag,
                                  const char *tag, const char *date,
                                  const char *fallback_previous) {
  if (previous_tag != NULL)
    return append_compare_heading(config, output, base, version, previous_tag,
                                  tag, date);
  /* Upstream back-fills the compare base for a section that has no older
   * release tag: the oldest commit in the section's own range becomes
   * previousTag and the compare link is still emitted, even when the host
   * base is empty (yielding a ///compare/... link). */
  if (fallback_previous != NULL)
    return append_compare_heading(config, output, base, version,
                                  fallback_previous, tag, date);
  if (csemver_preset_is_angular(config))
    return csemver_buffer_appendf(output, "%s %s (%s)\n\n",
                                  release_heading_level(config, version),
                                  version, date);
  return csemver_buffer_appendf(output, "## %s (%s)\n\n", version, date);
}

int csemver_normalize_changelog_newlines(CsemverBuffer *output) {
  while (output->length > 0 && output->data[output->length - 1] == '\n')
    output->data[--output->length] = '\0';
  return output->length == 0 || csemver_buffer_append(output, "\n", 1);
}

int csemver_regenerate_all_changelogs(
    const CsemverConfig *config, const char *version, const char *previous_tag,
    const char *new_tag, const Commit *commits, size_t commit_count,
    char tags[][SEMVER_TEXT_MAX], size_t tag_count, size_t history_limit,
    const char *date, const char *base, CsemverBuffer *output) {
  Commit *historical = calloc(CSEMVER_COMMIT_MAX, sizeof(*historical));
  bool wrote_section = false;
  size_t i, history_count = 0;
  if (historical == NULL)
    return 0;
  if (previous_tag == NULL || strcmp(previous_tag, new_tag) != 0) {
    if (!csemver_append_release_heading(config, output, base, version, previous_tag,
                                new_tag, date, NULL) ||
        !csemver_changelog_section(config, commits, commit_count, output))
      goto fail;
    wrote_section = true;
  }
  for (i = 0; i < tag_count; ++i) {
    char current_version[SEMVER_TEXT_MAX];
    char current_date[32];
    const char *older_tag = NULL;
    size_t j, historical_count = 0;
    if (history_limit != 0 && history_count >= history_limit)
      break;
    if (!stable_tag_version(config, tags[i], current_version,
                            sizeof current_version))
      continue;
    for (j = i + 1; j < tag_count; ++j) {
      char ignored_version[SEMVER_TEXT_MAX];
      if (stable_tag_version(config, tags[j], ignored_version,
                             sizeof ignored_version)) {
        older_tag = tags[j];
        break;
      }
    }
    if (!csemver_read_commits_range(config, older_tag, tags[i], historical,
                            &historical_count))
      goto fail;
    if (!get_tag_date(tags[i], current_date, sizeof current_date))
      snprintf(current_date, sizeof current_date, "%s", date);
    if (wrote_section &&
        !(output->length >= 2 && output->data[output->length - 1] == '\n' &&
          output->data[output->length - 2] == '\n') &&
        !csemver_buffer_append(output, "\n", 1))
      goto fail;
    if (!csemver_append_release_heading(
            config, output, base, current_version, older_tag, tags[i],
            current_date,
            historical_count > 0 ? historical[historical_count - 1].hash
                                 : NULL) ||
        !csemver_changelog_section(config, historical, historical_count, output))
      goto fail;
    wrote_section = true;
    ++history_count;
  }
  free(historical);
  return 1;
fail:
  free(historical);
  return 0;
}

int csemver_render_changelog(const CsemverConfig *config, const char *version,
                            const char *previous_tag, const char *new_tag,
                            const Commit *commits, size_t commit_count,
                            char tags[][SEMVER_TEXT_MAX], size_t tag_count,
                            CsemverBuffer *output) {
  char *old_content = NULL;
  char base[1024];
  const char *old_body = "";
  const char *line;
  char date[32];
  time_t now = time(NULL);
  struct tm utc;
  size_t old_length = 0;
  size_t front_matter_length = 0;
  repository_base(base, sizeof base);
  if (!config->dry_run && config->release_count != 0 &&
      csemver_read_file(config->infile, &old_content, &old_length)) {
    old_body = old_content;
    line = old_content;
    while (*line != '\0') {
      if ((strncmp(line, "## ", 3) == 0 || strncmp(line, "<a name=", 8) == 0)) {
        old_body = line;
        break;
      }
      line = strchr(line, '\n');
      if (line == NULL)
        break;
      ++line;
    }
    if (old_body != old_content && strncmp(old_content, "---\n", 4) == 0) {
      const char *closing = strstr(old_content + 4, "\n---\n");
      if (closing != NULL) {
        const char *header = closing + 5;
        while (*header == '\n' || *header == '\r')
          ++header;
        if (header < old_body && strncmp(header, "# ", 2) == 0)
          front_matter_length = (size_t)(closing + 5 - old_content);
      }
    }
  }
  gmtime_r(&now, &utc);
  strftime(date, sizeof date, "%Y-%m-%d", &utc);
  if (!config->dry_run && front_matter_length > 0) {
    if (!csemver_buffer_append(output, old_content, front_matter_length) ||
        (config->header[0] != '\0' && !csemver_buffer_append(output, "\n", 1)))
      goto fail;
  }
  if (!config->dry_run &&
      ((config->header[0] != '\0' &&
        !csemver_buffer_append(output, config->header,
                               strlen(config->header))) ||
       !csemver_buffer_append(output, "\n", 1)))
    goto fail;
  if (config->release_count == 0) {
    if (!csemver_regenerate_all_changelogs(config, version, previous_tag, new_tag,
                                   commits, commit_count, tags, tag_count, 0,
                                   date, base, output) ||
        !csemver_normalize_changelog_newlines(output))
      goto fail;
    free(old_content);
    return 1;
  }
  if (config->release_count > 1) {
    size_t history_limit = (size_t)config->release_count - 1;
    if (!csemver_regenerate_all_changelogs(config, version, previous_tag, new_tag,
                                   commits, commit_count, tags, tag_count,
                                   history_limit, date, base, output))
      goto fail;
    /* The regenerated sections already end with the template's trailing
     * blank, so the retained history is appended without another one. */
    if (*old_body != '\0' &&
        !csemver_buffer_append(output, old_body,
                               old_length - (size_t)(old_body - old_content)))
      goto fail;
    if (!csemver_normalize_changelog_newlines(output))
      goto fail;
    free(old_content);
    return 1;
  }
  if (previous_tag != NULL && strcmp(previous_tag, new_tag) == 0) {
    if (!config->dry_run &&
        (!csemver_buffer_append(output, "\n", 1) ||
         (*old_body != '\0' &&
          !csemver_buffer_append(output, old_body,
                                 old_length -
                                     (size_t)(old_body - old_content)))))
      goto fail;
    if ((*old_body != '\0' || config->dry_run) &&
        !csemver_normalize_changelog_newlines(output))
      goto fail;
    free(old_content);
    return 1;
  }
  if (previous_tag != NULL) {
    if (!append_compare_heading(config, output, base, version, previous_tag,
                                new_tag, date))
      goto fail;
  } else if (config->first_release) {
    if (!csemver_append_release_heading(config, output, base, version, NULL, new_tag,
                                date, NULL))
      goto fail;
  } else if (!csemver_append_release_heading(config, output, base, version, NULL,
                                     new_tag, date, NULL))
    goto fail;
  if (!csemver_changelog_section(config, commits, commit_count, output))
    goto fail;
  if (*old_body != '\0' &&
      (!csemver_buffer_append(output, "\n", 1) ||
       !csemver_buffer_append(output, old_body,
                              old_length - (size_t)(old_body - old_content))))
    goto fail;
  if (!csemver_normalize_changelog_newlines(output))
    goto fail;
  free(old_content);
  return 1;
fail:
  free(old_content);
  return 0;
}
