#define _POSIX_C_SOURCE 200809L
#include "release.h"

#include "common.h"
#include "config.h"
#include "semver.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef CSEMVER_VERSION
#define CSEMVER_VERSION "0.1.0"
#endif

#define ARG_MAX_COUNT 64
#define COMMIT_MAX 1024

typedef struct {
  char hash[64];
  char subject[2048];
  char body[4096];
} Commit;

static char *trim(char *text);
static int render_changelog(const CsemverConfig *config, const char *version,
                            const char *previous_tag, const char *new_tag,
                            const Commit *commits, size_t commit_count,
                            char tags[][SEMVER_TEXT_MAX], size_t tag_count,
                            CsemverBuffer *output);

static void errorf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fputs("csemver: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}

static void print_help(void) {
  puts("Usage: csemver [options]\n\n"
       "Options:\n"
       "  -h, --help                 Show this help\n"
       "  -v, --version              Show version\n"
       "  -r, --release-as VERSION   Release type or exact SemVer\n"
       "  -p, --prerelease [ID]      Create a prerelease\n"
       "  -f, --first-release        Tag the current version without bumping\n"
       "  -t, --tag-prefix PREFIX    Git tag prefix (default: v)\n"
       "  -i, --infile FILE          Changelog path (default: CHANGELOG.md)\n"
       "  -c, --config FILE          Read configuration from TOML\n"
       "      --dry-run              Preview without modifying the repository\n"
       "  -n, --no-verify            Bypass git commit hooks\n"
       "  -a, --commit-all           Include all staged and working files\n"
       "      --skip STEP            Skip bump, changelog, commit, or tag\n"
       "      --path PATH            Include commits under this path\n"
       "      --packageFiles FILE... Override package version files\n"
       "      --bumpFiles FILE...    Override files to update\n"
       "      --issuePrefixes PFX... Issue prefixes to link\n"
       "      --release-count N      Changelog count (1 latest, 0 all)\n"
       "  -s, --sign                 Sign release commit and tag\n"
       "      --signoff              Add a DCO signoff\n"
       "  -m, --message FORMAT       Deprecated; use --releaseCommitMessageFormat\n"
       "      --releaseCommitMessageFormat FORMAT\n"
       "      --header TEXT          Set changelog heading\n"
       "      --tag-force            Replace an existing tag\n"
       "      --git-tag-fallback     Read version from a tag if no file exists\n"
       "      --noBumpWhenEmptyChanges\n"
       "      --silent               Suppress normal progress output\n");
}

static int run_command(const char *const argv[], char **output, int *status) {
  return csemver_run_process(argv, output, status);
}

static int run_git(const char *const args[], char **output, int *status) {
  const char *argv[ARG_MAX_COUNT];
  size_t count = 0;
  while (args[count] != NULL && count + 2 < ARG_MAX_COUNT) {
    argv[count + 1] = args[count];
    ++count;
  }
  argv[0] = "git";
  argv[count + 1] = NULL;
  if (count + 2 >= ARG_MAX_COUNT)
    return 0;
  return run_command(argv, output, status);
}

static int load_config(CsemverConfig *config, const char *path) {
  char *contents = NULL;
  char error[256] = {0};
  if (path == NULL) {
    if (!csemver_read_file("csemver.toml", &contents, NULL))
      return 1;
  } else if (!csemver_read_file(path, &contents, NULL)) {
    errorf("cannot read TOML config '%s'", path);
    return 0;
  }
  if (!csemver_config_parse(config, contents, error, sizeof error)) {
    errorf("%s", error);
    free(contents);
    return 0;
  }
  free(contents);
  return 1;
}

static int parse_args(int argc, char **argv, CsemverConfig *config,
                      const char **config_path) {
  int i;
  for (i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = NULL;
    char key[128];
    const char *equals = strchr(arg, '=');
    size_t key_len = equals == NULL ? strlen(arg) : (size_t)(equals - arg);
    if (key_len >= sizeof key) {
      errorf("option name is too long");
      return 2;
    }
    memcpy(key, arg, key_len);
    key[key_len] = '\0';
    if (equals != NULL)
      value = equals + 1;
    if (strcmp(key, "--help") == 0 || strcmp(key, "-h") == 0) {
      print_help();
      return 1;
    }
    if (strcmp(key, "--version") == 0 || strcmp(key, "-v") == 0) {
      puts("csemver " CSEMVER_VERSION);
      return 1;
    }
    if (strcmp(key, "-c") == 0 || strcmp(key, "--config") == 0) {
      if (value == NULL && i + 1 < argc)
        value = argv[++i];
      if (value == NULL) {
        errorf("%s requires a file path", key);
        return 2;
      }
      *config_path = value;
      continue;
    }
    if (strcmp(key, "-p") == 0 || strcmp(key, "--prerelease") == 0) {
      const char *id = value;
      if (id == NULL && i + 1 < argc && argv[i + 1][0] != '-')
        id = argv[++i];
      config->has_prerelease = true;
      if (id != NULL &&
          !csemver_config_set_string(config, "prerelease", id, NULL, 0))
        return 2;
      continue;
    }
    if (strcmp(key, "--release-as") == 0 || strcmp(key, "-r") == 0 ||
        strcmp(key, "--infile") == 0 || strcmp(key, "-i") == 0 ||
        strcmp(key, "--tag-prefix") == 0 || strcmp(key, "-t") == 0 ||
        strcmp(key, "--path") == 0 || strcmp(key, "--preset") == 0 ||
        strcmp(key, "--message") == 0 || strcmp(key, "-m") == 0 ||
        strcmp(key, "--releaseCommitMessageFormat") == 0 ||
        strcmp(key, "--header") == 0 || strcmp(key, "--changelogHeader") == 0 ||
        strcmp(key, "--lerna-package") == 0 ||
        strcmp(key, "--npmPublishHint") == 0) {
      char error[256] = {0};
      const char *name = strcmp(key, "-r") == 0   ? "release-as"
                         : strcmp(key, "-i") == 0 ? "infile"
                         : strcmp(key, "-t") == 0 ? "tag-prefix"
                         : strcmp(key, "-m") == 0 ? "message"
                                                  : key + 2;
      if (value == NULL && i + 1 < argc)
        value = argv[++i];
      if (value == NULL) {
        errorf("%s requires a value", key);
        return 2;
      }
      if (strcmp(key, "--path") == 0)
        name = "path";
      if (strcmp(key, "--preset") == 0)
        name = "preset";
      if (strcmp(key, "--releaseCommitMessageFormat") == 0)
        name = "releaseCommitMessageFormat";
      if (strcmp(key, "--header") == 0 || strcmp(key, "--changelogHeader") == 0)
        name = "header";
      if (strcmp(key, "--lerna-package") == 0)
        name = "lerna-package";
      if (strcmp(key, "--npmPublishHint") == 0)
        name = "npmPublishHint";
      if (!csemver_config_set_string(config, name, value, error,
                                     sizeof error)) {
        errorf("%s", error);
        return 2;
      }
      continue;
    }
    if (strcmp(key, "--packageFiles") == 0 ||
        strcmp(key, "--package-files") == 0 ||
        strcmp(key, "--bumpFiles") == 0 || strcmp(key, "--bump-files") == 0 ||
        strcmp(key, "--issuePrefixes") == 0 ||
        strcmp(key, "--issue-prefixes") == 0) {
      const char *values[CSEMVER_MAX_FILES];
      const char *name = key + 2;
      size_t count = 0;
      char error[256] = {0};
      if (value != NULL)
        values[count++] = value;
      while (i + 1 < argc && argv[i + 1][0] != '-') {
        if (count >= CSEMVER_MAX_FILES) {
          errorf("too many values for %s", key);
          return 2;
        }
        values[count++] = argv[++i];
      }
      if (!csemver_config_set_array(config, name, values, count, error,
                                    sizeof error)) {
        errorf("%s", error);
        return 2;
      }
      continue;
    }
    if (strcmp(key, "--release-count") == 0 ||
        strcmp(key, "--releaseCount") == 0) {
      char *end = NULL;
      long count;
      if (value == NULL && i + 1 < argc)
        value = argv[++i];
      if (value == NULL) {
        errorf("%s requires an integer", key);
        return 2;
      }
      errno = 0;
      count = strtol(value, &end, 10);
      if (errno != 0 || *end != '\0' || count < 0 || count > UINT_MAX) {
        errorf("invalid release count '%s'", value);
        return 2;
      }
      config->release_count = (unsigned)count;
      continue;
    }
    if (strcmp(key, "--skip") == 0) {
      if (value == NULL && i + 1 < argc)
        value = argv[++i];
      if (value == NULL) {
        errorf("--skip requires a step name");
        return 2;
      }
      if (strcmp(value, "bump") == 0)
        config->skip_bump = true;
      else if (strcmp(value, "changelog") == 0)
        config->skip_changelog = true;
      else if (strcmp(value, "commit") == 0)
        config->skip_commit = true;
      else if (strcmp(value, "tag") == 0)
        config->skip_tag = true;
      else {
        errorf("unknown skip step '%s'", value);
        return 2;
      }
      continue;
    }
    if (strcmp(key, "--dry-run") == 0 || strcmp(key, "--first-release") == 0 ||
        strcmp(key, "-f") == 0 || strcmp(key, "--sign") == 0 ||
        strcmp(key, "-s") == 0 || strcmp(key, "--signoff") == 0 ||
        strcmp(key, "--no-verify") == 0 || strcmp(key, "-n") == 0 ||
        strcmp(key, "--commit-all") == 0 || strcmp(key, "-a") == 0 ||
        strcmp(key, "--silent") == 0 || strcmp(key, "--tag-force") == 0 ||
        strcmp(key, "--git-tag-fallback") == 0 ||
        strcmp(key, "--noBumpWhenEmptyChanges") == 0 ||
        strcmp(key, "--no-bump-when-empty-changes") == 0 ||
        strncmp(key, "--skip.", 7) == 0) {
      const char *name = key;
      bool flag = true;
      char error[256] = {0};
      if (value == NULL && i + 1 < argc &&
          (strcmp(argv[i + 1], "true") == 0 ||
           strcmp(argv[i + 1], "false") == 0))
        value = argv[++i];
      if (value != NULL)
        flag = strcmp(value, "false") != 0;
      if (strcmp(key, "-f") == 0)
        name = "first-release";
      else if (strcmp(key, "--first-release") == 0)
        name = "first-release";
      else if (strcmp(key, "--no-verify") == 0 || strcmp(key, "-n") == 0)
        name = "no-verify";
      else if (strcmp(key, "--commit-all") == 0 || strcmp(key, "-a") == 0)
        name = "commit-all";
      else if (strcmp(key, "-s") == 0)
        name = "sign";
      else if (strncmp(key, "--skip.", 7) == 0)
        name = key + 2;
      else
        name = key + 2;
      if (!csemver_config_set_bool(config, name, flag, error, sizeof error)) {
        errorf("%s", error);
        return 2;
      }
      continue;
    }
    errorf("unknown option '%s'", arg);
    return 2;
  }
  return 0;
}

/* RELEASE_ENGINE */

static int collect_tags(const CsemverConfig *config,
                        char tags[][SEMVER_TEXT_MAX], size_t *tag_count,
                        char *latest_version, char *latest_tag) {
  const char *args[] = {"tag", "--list", "--sort=-version:refname", NULL};
  char *output = NULL;
  char *line;
  char *save = NULL;
  int status = 0;
  Semver latest = {0};
  bool found = false;
  *tag_count = 0;
  latest_version[0] = latest_tag[0] = '\0';
  if (!run_git(args, &output, &status) || status != 0) {
    free(output);
    return 0;
  }
  line = strtok_r(output, "\n", &save);
  while (line != NULL) {
    char candidate[SEMVER_TEXT_MAX];
    Semver parsed;
    if (strncmp(line, config->tag_prefix, strlen(config->tag_prefix)) == 0 &&
        strlen(line) - strlen(config->tag_prefix) < sizeof candidate) {
      snprintf(candidate, sizeof candidate, "%s",
               line + strlen(config->tag_prefix));
      if (semver_parse(candidate, &parsed)) {
        if (*tag_count < COMMIT_MAX)
          snprintf(tags[(*tag_count)++], SEMVER_TEXT_MAX, "%s", line);
        if (!found || semver_compare(candidate, latest_version) > 0) {
          latest = parsed;
          snprintf(latest_version, SEMVER_TEXT_MAX, "%s", candidate);
          snprintf(latest_tag, SEMVER_TEXT_MAX, "%s", line);
          found = true;
        }
      }
    }
    line = strtok_r(NULL, "\n", &save);
  }
  free(output);
  (void)latest;
  return 1;
}

static int get_version(const CsemverConfig *config, char *version,
                       bool *is_private) {
  size_t index;
  for (index = 0; index < config->package_file_count; ++index) {
    char *content = NULL;
    char error[256];
    if (!csemver_read_file(config->package_files[index].filename, &content,
                           NULL))
      continue;
    if (csemver_version_read_text(config->package_files[index].filename,
                                  config->package_files[index].type, content,
                                  version, SEMVER_TEXT_MAX, is_private, error,
                                  sizeof error)) {
      free(content);
      return 1;
    }
    free(content);
    errorf("%s: %s", config->package_files[index].filename, error);
    return 0;
  }
  return 0;
}

static int read_commits_range(const CsemverConfig *config,
                              const char *previous_tag, const char *end_ref,
                              Commit *commits, size_t *commit_count);

static int read_commits(const CsemverConfig *config, const char *previous_tag,
                        Commit *commits, size_t *commit_count) {
  return read_commits_range(config, previous_tag, "HEAD", commits,
                            commit_count);
}

static int read_commits_range(const CsemverConfig *config,
                              const char *previous_tag, const char *end_ref,
                              Commit *commits, size_t *commit_count) {
  const char *args[ARG_MAX_COUNT];
  char range[SEMVER_TEXT_MAX * 2];
  char *output = NULL;
  int status = 0;
  size_t used = 0;
  char *cursor;
  args[used++] = "log";
  args[used++] = "--no-merges";
  args[used++] = "--format=%H%x1f%s%x1f%b%x1e";
  if (previous_tag != NULL && end_ref != NULL) {
    if (snprintf(range, sizeof range, "%s..%s", previous_tag, end_ref) >=
        (int)sizeof range)
      return 0;
    args[used++] = range;
  } else if (previous_tag != NULL) {
    if (snprintf(range, sizeof range, "%s..HEAD", previous_tag) >=
        (int)sizeof range)
      return 0;
    args[used++] = range;
  } else if (end_ref != NULL) {
    args[used++] = end_ref;
  }
  if (config->path[0] != '\0') {
    args[used++] = "--";
    args[used++] = config->path;
  }
  args[used] = NULL;
  *commit_count = 0;
  if (!run_git(args, &output, &status) || status != 0) {
    free(output);
    return 0;
  }
  cursor = output;
  while (*cursor != '\0' && *commit_count < COMMIT_MAX) {
    char *end;
    char *first;
    char *second;
    Commit *item;
    while (*cursor == '\n' || *cursor == '\r')
      ++cursor;
    if (*cursor == '\0')
      break;
    end = strchr(cursor, '\x1e');
    if (end == NULL)
      break;
    *end = '\0';
    first = strchr(cursor, '\x1f');
    if (first == NULL) {
      cursor = end + 1;
      continue;
    }
    *first++ = '\0';
    second = strchr(first, '\x1f');
    if (second == NULL) {
      cursor = end + 1;
      continue;
    }
    *second++ = '\0';
    item = &commits[(*commit_count)++];
    snprintf(item->hash, sizeof item->hash, "%s", cursor);
    snprintf(item->subject, sizeof item->subject, "%s", first);
    snprintf(item->body, sizeof item->body, "%s", trim(second));
    cursor = end + 1;
  }
  free(output);
  return 1;
}

static char *trim(char *text) {
  size_t length;
  while (*text != '\0' && isspace((unsigned char)*text))
    ++text;
  length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';
  return text;
}

static int type_index(const CsemverConfig *config, const char *type) {
  size_t i;
  for (i = 0; i < config->commit_type_count; ++i)
    if (strcmp(config->commit_types[i].type, type) == 0)
      return (int)i;
  return -1;
}

static int commit_is_breaking(const char *subject, const char *body) {
  const char *colon = strchr(subject, ':');
  const char *line;
  if (colon != NULL && colon > subject && colon[-1] == '!')
    return 1;
  line = body;
  while (*line != '\0') {
    while (*line == '\n' || *line == '\r' || *line == ' ')
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
  if (colon > subject && colon[-1] == '!')
    type[type_size - 1] = '\0';
  *description = colon + 1;
  while (**description == ' ')
    ++*description;
  return 1;
}

static int append_issue_link(CsemverBuffer *out, const CsemverConfig *config,
                             const char *text, const char *base) {
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
      if (digits > 0 && base[0] != '\0') {
        char id[32];
        size_t id_len = token_len + digits;
        if (id_len >= sizeof id)
          return 0;
        memcpy(id, text + i, id_len);
        id[id_len] = '\0';
        if (!csemver_buffer_appendf(out, "[%s](%s/issues/%s)", id, base,
                                    id + token_len))
          return 0;
        i += id_len;
        matched = true;
        break;
      }
    }
    if (!matched) {
      if (!csemver_buffer_append(out, text + i, 1))
        return 0;
      ++i;
    }
  }
  return 1;
}

static void repository_base(char *base, size_t base_size) {
  const char *args[] = {"config", "--get", "remote.origin.url", NULL};
  char *output = NULL;
  char *url;
  char *path;
  int status = 0;
  base[0] = '\0';
  if (!run_git(args, &output, &status) || status != 0 || output == NULL) {
    free(output);
    return;
  }
  url = trim(output);
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
  if (!conventional_type(commit->subject, type, sizeof type, scope,
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
  if (base[0] != '\0') {
    if (!csemver_buffer_appendf(section, " ([%s](%s/commit/%s))", short_hash,
                                base, commit->hash))
      return 0;
  } else if (!csemver_buffer_appendf(section, " %s", short_hash))
    return 0;
  return csemver_buffer_append(section, "\n", 1);
}

static int append_breaking_notes(CsemverBuffer *notes, const char *body) {
  const char *line = body;
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
      if (end > note &&
          (!csemver_buffer_append(notes, "* ", 2) ||
           !csemver_buffer_append(notes, note, (size_t)(end - note)) ||
           !csemver_buffer_append(notes, "\n", 1)))
        return 0;
    }
    line = strchr(line, '\n');
    if (line == NULL)
      break;
    ++line;
  }
  return 1;
}

static int changelog_section(const CsemverConfig *config, const Commit *commits,
                             size_t commit_count, CsemverBuffer *output) {
  CsemverBuffer groups[CSEMVER_MAX_TYPES];
  CsemverBuffer breaking;
  bool used[CSEMVER_MAX_TYPES] = {false};
  char base[1024];
  size_t i;
  csemver_buffer_init(&breaking);
  repository_base(base, sizeof base);
  for (i = 0; i < config->commit_type_count; ++i)
    csemver_buffer_init(&groups[i]);
  for (i = 0; i < commit_count; ++i) {
    char type[128], scope[256];
    const char *description;
    int index;
    if (!append_breaking_notes(&breaking, commits[i].body))
      goto fail;
    if (!conventional_type(commits[i].subject, type, sizeof type, scope,
                           sizeof scope, &description))
      continue;
    (void)description;
    index = type_index(config, type);
    if (index < 0 || config->commit_types[index].hidden ||
        config->commit_types[index].section[0] == '\0')
      continue;
    used[index] = true;
    if (!append_commit_line(&groups[index], config, &commits[i], base))
      goto fail;
  }
  if (breaking.length > 0) {
    if (!csemver_buffer_appendf(output, "### ⚠ BREAKING CHANGES\n\n") ||
        !csemver_buffer_append(output, breaking.data, breaking.length))
      goto fail;
  }
  {
    bool wrote_section = breaking.length > 0;
    for (i = 0; i < config->commit_type_count; ++i) {
      if (!used[i])
        continue;
      if ((wrote_section && !csemver_buffer_append(output, "\n", 1)) ||
          !csemver_buffer_appendf(output, "### %s\n\n",
                                  config->commit_types[i].section) ||
          !csemver_buffer_append(output, groups[i].data, groups[i].length))
        goto fail;
      wrote_section = true;
    }
  }
  for (i = 0; i < config->commit_type_count; ++i)
    csemver_buffer_free(&groups[i]);
  csemver_buffer_free(&breaking);
  return 1;
fail:
  for (i = 0; i < config->commit_type_count; ++i)
    csemver_buffer_free(&groups[i]);
  csemver_buffer_free(&breaking);
  return 0;
}

static int calculate_bump(const CsemverConfig *config, const Commit *commits,
                          size_t count, const Semver *current) {
  int bump = 0;
  size_t i;
  for (i = 0; i < count; ++i) {
    char type[128], scope[256];
    const char *description;
    int index;
    if (!conventional_type(commits[i].subject, type, sizeof type, scope,
                           sizeof scope, &description))
      continue;
    (void)description;
    if (commit_is_breaking(commits[i].subject, commits[i].body))
      return config->pre_major && current->major == 0 ? 2 : 3;
    index = type_index(config, type);
    if (index < 0 || config->commit_types[index].hidden)
      continue;
    if (strcmp(type, "feat") == 0 || strcmp(type, "feature") == 0)
      bump = bump < (current->major == 0 && config->pre_major ? 1 : 2)
                 ? (current->major == 0 && config->pre_major ? 1 : 2)
                 : bump;
    else if (config->commit_types[index].bump && bump < 1)
      bump = 1;
  }
  return bump;
}

static const char *bump_name(int bump) {
  return bump == 3 ? "major" : bump == 2 ? "minor" : "patch";
}

static int run_lifecycle_capture(const CsemverConfig *config, const char *name,
                                 char **output) {
  size_t i;
  if (output != NULL)
    *output = NULL;
  for (i = 0; i < config->script_count; ++i) {
    const char *argv[] = {"/bin/sh", "-c", config->scripts[i].command, NULL};
    int status = 0;
    char **capture = output != NULL && *output == NULL ? output : NULL;
    if (strcmp(config->scripts[i].name, name) != 0)
      continue;
    if (!run_command(argv, capture, &status) || status != 0) {
      errorf("lifecycle script '%s' failed with status %d", name, status);
      return 0;
    }
  }
  return 1;
}

static int run_lifecycle(const CsemverConfig *config, const char *name) {
  return run_lifecycle_capture(config, name, NULL);
}

static int prepare_bump(CsemverConfig *config) {
  char *output = NULL;
  char *text;
  char version[SEMVER_TEXT_MAX];
  size_t used = 0;
  Semver parsed;
  if (config->skip_bump || config->dry_run)
    return 1;
  if (!run_lifecycle(config, "prerelease") ||
      !run_lifecycle_capture(config, "prebump", &output)) {
    free(output);
    return 0;
  }
  if (output == NULL) {
    return 1;
  }
  text = trim(output);
  while (*text != '\0' && used + 1 < sizeof version) {
    if (*text != '\'' && *text != '"')
      version[used++] = *text;
    ++text;
  }
  version[used] = '\0';
  free(output);
  if (used > 0 && semver_parse(version, &parsed)) {
    if (snprintf(config->release_as, sizeof config->release_as, "%s",
                 version) >= (int)sizeof config->release_as) {
      errorf("prebump version is too long");
      return 0;
    }
  }
  return 1;
}

static int release_message(const CsemverConfig *config, const char *version,
                           char *message, size_t message_size) {
  const char *format = config->has_message
                           ? config->message
                           : config->release_commit_message_format;
  CsemverBuffer output;
  size_t i;
  csemver_buffer_init(&output);
  for (i = 0; format[i] != '\0';) {
    if (config->has_message && strncmp(format + i, "%s", 2) == 0) {
      if (!csemver_buffer_append(&output, version, strlen(version)))
        goto fail;
      i += 2;
    } else if (strncmp(format + i, "{{currentTag}}", 14) == 0) {
      if (!csemver_buffer_append(&output, version, strlen(version)))
        goto fail;
      i += 14;
    } else {
      if (!csemver_buffer_append(&output, format + i, 1))
        goto fail;
      ++i;
    }
  }
  if (output.length >= message_size)
    goto fail;
  memcpy(message, output.data == NULL ? "" : output.data, output.length + 1);
  csemver_buffer_free(&output);
  return 1;
fail:
  csemver_buffer_free(&output);
  return 0;
}

static int generate_version(const CsemverConfig *config, const char *current,
                            int bump, char *next, size_t next_size) {
  Semver parsed;
  char type[32];
  if (!semver_parse(current, &parsed))
    return 0;
  if (config->first_release && config->release_as[0] == '\0')
    return semver_format(&parsed, next, next_size);
  if (config->release_as[0] != '\0') {
    if (semver_parse(config->release_as, &parsed)) {
      return semver_format(&parsed, next, next_size);
    }
    if (config->has_prerelease) {
      if (snprintf(type, sizeof type, "pre%s", config->release_as) >=
          (int)sizeof type)
        return 0;
      return semver_bump(&parsed, type, config->prerelease_id, next, next_size);
    }
    return semver_bump(&parsed, config->release_as, NULL, next, next_size);
  }
  if (bump == 0) {
    if (config->no_bump_when_empty_changes)
      return semver_format(&parsed, next, next_size);
    return 0;
  }
  if (config->has_prerelease) {
    if (parsed.has_prerelease)
      return semver_bump(&parsed, "prerelease", config->prerelease_id, next,
                         next_size);
    if (snprintf(type, sizeof type, "pre%s", bump_name(bump)) >=
        (int)sizeof type)
      return 0;
    return semver_bump(&parsed, type, config->prerelease_id, next, next_size);
  }
  return semver_bump(&parsed, bump_name(bump), NULL, next, next_size);
}

static int update_files(const CsemverConfig *config, const char *version,
                        char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                        size_t *path_count) {
  size_t i;
  *path_count = 0;
  if (config->skip_bump)
    return 1;
  if (config->first_release)
    return run_lifecycle(config, "postbump");
  for (i = 0; i < config->bump_file_count; ++i) {
    char *content = NULL;
    char *updated = NULL;
    char old_version[SEMVER_TEXT_MAX];
    char error[256] = {0};
    size_t updated_size = 0;
    if (!csemver_read_file(config->bump_files[i].filename, &content, NULL))
      continue;
    if (!csemver_version_update_text(
            config->bump_files[i].filename, config->bump_files[i].type, content,
            version, &updated, &updated_size, old_version, sizeof old_version,
            error, sizeof error)) {
      errorf("%s: %s", config->bump_files[i].filename, error);
      free(content);
      return 0;
    }
    if (!csemver_write_file(config->bump_files[i].filename, updated,
                            updated_size)) {
      errorf("cannot write %s", config->bump_files[i].filename);
      free(content);
      free(updated);
      return 0;
    }
    free(content);
    free(updated);
    if (*path_count < CSEMVER_MAX_FILES)
      snprintf(paths[(*path_count)++], CSEMVER_PATH_MAX, "%s",
               config->bump_files[i].filename);
  }
  return run_lifecycle(config, "postbump");
}

static int write_changelog(const CsemverConfig *config, const char *version,
                           const char *previous_tag, const char *new_tag,
                           const Commit *commits, size_t commit_count,
                           char tags[][SEMVER_TEXT_MAX], size_t tag_count,
                           char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                           size_t *path_count) {
  CsemverBuffer content;
  int ok;
  if (config->skip_changelog)
    return 1;
  if (!config->dry_run && !run_lifecycle(config, "prechangelog"))
    return 0;
  if (!config->silent && access(config->infile, F_OK) != 0)
    printf("✔ created %s\n", config->infile);
  csemver_buffer_init(&content);
  if (!render_changelog(config, version, previous_tag, new_tag, commits,
                        commit_count, tags, tag_count, &content)) {
    csemver_buffer_free(&content);
    errorf("failed to generate changelog");
    return 0;
  }
  if (!config->silent)
    printf("✔ outputting changes to %s\n", config->infile);
  if (config->dry_run) {
    char *preview = content.data == NULL ? NULL : trim(content.data);
    printf("\n---\n%s\n---\n\n", preview == NULL ? "" : preview);
    ok = 1;
  } else {
    ok = csemver_write_file(config->infile,
                            content.data == NULL ? "" : content.data,
                            content.length);
    if (!ok)
      errorf("cannot write changelog '%s'", config->infile);
  }
  if (ok && *path_count < CSEMVER_MAX_FILES + 1)
    snprintf(paths[(*path_count)++], CSEMVER_PATH_MAX, "%s", config->infile);
  csemver_buffer_free(&content);
  if (ok && !config->dry_run)
    ok = run_lifecycle(config, "postchangelog");
  return ok;
}

static int commit_release(CsemverConfig *config, const char *version,
                          char *message, size_t message_size,
                          char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                          size_t path_count) {
  const char *args[ARG_MAX_COUNT];
  char *hook_message = NULL;
  size_t index = 0;
  int status = 0;
  if (config->skip_commit)
    return 1;
  if (path_count == 0 && !config->commit_all)
    return 1;
  if (!run_lifecycle_capture(config, "precommit", &hook_message)) {
    free(hook_message);
    return 0;
  }
  if (hook_message != NULL && trim(hook_message)[0] != '\0') {
    const char *format = trim(hook_message);
    if (strlen(format) >= sizeof config->release_commit_message_format) {
      errorf("precommit message is too long");
      free(hook_message);
      return 0;
    }
    snprintf(config->release_commit_message_format,
             sizeof config->release_commit_message_format, "%s", format);
    config->has_message = false;
    if (!release_message(config, version, message, message_size)) {
      free(hook_message);
      errorf("cannot format precommit message");
      return 0;
    }
  }
  free(hook_message);
  if (config->commit_all) {
    args[index++] = "add";
    args[index++] = "-A";
    args[index] = NULL;
    if (!run_git(args, NULL, &status) || status != 0)
      return 0;
    index = 0;
  } else if (path_count > 0) {
    args[index++] = "add";
    args[index++] = "--";
    for (size_t i = 0; i < path_count && index + 1 < ARG_MAX_COUNT; ++i)
      args[index++] = paths[i];
    args[index] = NULL;
    if (!run_git(args, NULL, &status) || status != 0)
      return 0;
    index = 0;
  }
  args[index++] = "commit";
  if (config->sign)
    args[index++] = "-S";
  if (config->signoff)
    args[index++] = "-s";
  if (config->no_verify)
    args[index++] = "--no-verify";
  args[index++] = "-m";
  args[index++] = message;
  if (!config->commit_all) {
    for (size_t i = 0; i < path_count && index + 1 < ARG_MAX_COUNT; ++i)
      args[index++] = paths[i];
  }
  args[index] = NULL;
  if (!run_git(args, NULL, &status) || status != 0) {
    errorf("git commit failed");
    return 0;
  }
  return run_lifecycle(config, "postcommit");
}

static int tag_release(const CsemverConfig *config, const char *tag,
                       const char *message) {
  const char *args[8];
  size_t index = 0;
  int status = 0;
  if (config->skip_tag)
    return 1;
  if (!run_lifecycle(config, "pretag"))
    return 0;
  args[index++] = "tag";
  if (config->tag_force)
    args[index++] = "--force";
  if (config->sign)
    args[index++] = "-s";
  else
    args[index++] = "-a";
  args[index++] = "-m";
  args[index++] = message;
  args[index++] = tag;
  args[index] = NULL;
  if (!run_git(args, NULL, &status) || status != 0) {
    errorf("git tag failed for %s", tag);
    return 0;
  }
  return run_lifecycle(config, "posttag");
}

static int
print_publish_hint(const CsemverConfig *config, bool is_private,
                   char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                   size_t path_count) {
  const char *branch_args[] = {"rev-parse", "--abbrev-ref", "HEAD", NULL};
  char *branch_output = NULL;
  const char *publish_command = config->npm_publish_hint;
  size_t i;
  int status = 0;
  bool updated_package = false;
  if (config->silent || config->skip_tag || config->skip_bump || is_private)
    return 1;
  for (i = 0; i < path_count; ++i)
    if (strcmp(paths[i], "package.json") == 0)
      updated_package = true;
  if (!updated_package)
    return 1;
  if (!run_git(branch_args, &branch_output, &status) || status != 0 ||
      branch_output == NULL) {
    free(branch_output);
    return 1;
  }
  if (publish_command[0] == '\0') {
    if (access("yarn.lock", F_OK) == 0)
      publish_command = "yarn publish";
    else if (access("pnpm-lock.yaml", F_OK) == 0)
      publish_command = "pnpm publish";
    else
      publish_command = "npm publish";
  }
  printf("ℹ Run `git push --follow-tags origin %s && %s", trim(branch_output),
         publish_command);
  free(branch_output);
  if (config->has_prerelease)
    printf(" --tag %s", config->prerelease_id[0] == '\0'
                            ? "prerelease"
                            : config->prerelease_id);
  puts("` to publish");
  return 1;
}

static const char *find_config_path(int argc, char **argv, char *storage,
                                    size_t storage_size) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
      if (i + 1 < argc)
        return argv[i + 1];
      return NULL;
    }
    if (strncmp(argv[i], "--config=", 9) == 0) {
      snprintf(storage, storage_size, "%s", argv[i] + 9);
      return storage;
    }
  }
  return NULL;
}

int csemver_main(int argc, char **argv) {
  CsemverConfig config;
  const char *config_path;
  char config_storage[CSEMVER_PATH_MAX];
  char tags[COMMIT_MAX][SEMVER_TEXT_MAX];
  char latest_version[SEMVER_TEXT_MAX], latest_tag[SEMVER_TEXT_MAX];
  char current[SEMVER_TEXT_MAX], next[SEMVER_TEXT_MAX];
  char new_tag[SEMVER_TEXT_MAX];
  char message[CSEMVER_VALUE_MAX];
  bool is_private = false;
  size_t tag_count = 0, commit_count = 0, path_count = 0;
  Commit *commits = NULL;
  char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX];
  Semver current_semver;
  int bump, parsed_args;
  char *check_output = NULL;
  int status = 0;
  config_path =
      find_config_path(argc, argv, config_storage, sizeof config_storage);
  csemver_config_defaults(&config);
  if (!load_config(&config, config_path))
    return 2;
  parsed_args = parse_args(argc, argv, &config, &config_path);
  if (parsed_args != 0)
    return parsed_args == 1 ? 0 : parsed_args;
  {
    const char *args[] = {"rev-parse", "--is-inside-work-tree", NULL};
    if (!run_git(args, &check_output, &status) || status != 0 ||
        check_output == NULL || strncmp(check_output, "true", 4) != 0) {
      free(check_output);
      errorf("not inside a Git working tree");
      return 1;
    }
    free(check_output);
  }
  if (!collect_tags(&config, tags, &tag_count, latest_version, latest_tag)) {
    errorf("cannot inspect Git tags");
    return 1;
  }
  if (!get_version(&config, current, &is_private)) {
    if (config.git_tag_fallback && latest_version[0] != '\0')
      snprintf(current, sizeof current, "%s", latest_version);
    else
      snprintf(current, sizeof current, "1.0.0");
  }
  if (!prepare_bump(&config))
    return 1;
  if (!semver_parse(current, &current_semver)) {
    errorf("invalid current version '%s'", current);
    return 1;
  }
  commits = calloc(COMMIT_MAX, sizeof(*commits));
  if (commits == NULL) {
    errorf("out of memory");
    return 1;
  }
  if (!read_commits(&config, latest_tag[0] == '\0' ? NULL : latest_tag, commits,
                    &commit_count)) {
    free(commits);
    errorf("cannot read Git history");
    return 1;
  }
  bump = calculate_bump(&config, commits, commit_count, &current_semver);
  if (config.first_release) {
    bump = 0;
    snprintf(next, sizeof next, "%s", current);
  } else if (config.skip_bump) {
    snprintf(next, sizeof next, "%s", current);
  } else if (config.release_as[0] == '\0' && bump == 0 &&
             config.no_bump_when_empty_changes) {
    if (!config.silent)
      puts("✔ no commits found, so not bumping version");
    free(commits);
    return 0;
  } else {
    if (config.release_as[0] == '\0' && bump == 0)
      bump = 1;
    if (!generate_version(&config, current, bump, next, sizeof next)) {
      free(commits);
      errorf(
          "no releasable conventional commits found, or invalid release type");
      return 1;
    }
  }
  if (snprintf(new_tag, sizeof new_tag, "%s%s", config.tag_prefix, next) >=
          (int)sizeof new_tag ||
      !release_message(&config, next, message, sizeof message)) {
    free(commits);
    errorf("release version or message is too long");
    return 1;
  }
  if (!config.silent) {
    if (config.first_release)
      puts("✖ skip version bump on first release");
    else if (!config.skip_bump) {
      for (size_t i = 0; i < config.bump_file_count; ++i) {
        char *contents = NULL, old[SEMVER_TEXT_MAX], error[256];
        if (!csemver_read_file(config.bump_files[i].filename, &contents, NULL))
          continue;
        if (csemver_version_read_text(config.bump_files[i].filename,
                                      config.bump_files[i].type, contents, old,
                                      sizeof old, NULL, error, sizeof error))
          printf("✔ bumping version in %s from %s to %s\n",
                 config.bump_files[i].filename, old, next);
        free(contents);
      }
    }
  }
  if (!config.dry_run && !update_files(&config, next, paths, &path_count)) {
    free(commits);
    return 1;
  }
  if (!config.dry_run &&
      !read_commits(&config, latest_tag[0] == '\0' ? NULL : latest_tag, commits,
                    &commit_count)) {
    free(commits);
    errorf("cannot reread Git history after release hooks");
    return 1;
  }
  if (config.dry_run && !config.skip_bump && !config.first_release) {
    for (size_t i = 0; i < config.bump_file_count; ++i)
      if (access(config.bump_files[i].filename, F_OK) == 0)
        snprintf(paths[path_count++], CSEMVER_PATH_MAX, "%s",
                 config.bump_files[i].filename);
  }
  if (!config.skip_changelog &&
      !write_changelog(&config, next, latest_tag[0] == '\0' ? NULL : latest_tag,
                       new_tag, commits, commit_count, tags, tag_count, paths,
                       &path_count)) {
    free(commits);
    return 1;
  }
  if (!config.dry_run) {
    if (!commit_release(&config, next, message, sizeof message, paths,
                        path_count) ||
        !tag_release(&config, new_tag, message)) {
      free(commits);
      return 1;
    }
  }
  if (!config.silent) {
    if (!config.skip_commit) {
      fputs("✔ committing ", stdout);
      if (config.commit_all)
        fputs("all staged files", stdout);
      else
        for (size_t i = 0; i < path_count; ++i) {
          if (i != 0)
            fputs(" and ", stdout);
          fputs(paths[i], stdout);
        }
      fputc('\n', stdout);
    }
    if (!config.skip_tag)
      printf("✔ tagging release %s\n", new_tag);
  }
  (void)print_publish_hint(&config, is_private, paths, path_count);
  free(commits);
  (void)tag_count;
  (void)is_private;
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
  if (!run_git(args, &output, &status) || status != 0 || output == NULL) {
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

static int append_release_heading(CsemverBuffer *output, const char *base,
                                  const char *version, const char *previous_tag,
                                  const char *tag, const char *date,
                                  bool initial_release) {
  if (base[0] != '\0' && previous_tag != NULL)
    return csemver_buffer_appendf(output,
                                  "## [%s](%s/compare/%s...%s) (%s)\n\n",
                                  version, base, previous_tag, tag, date);
  if (initial_release)
    return csemver_buffer_appendf(output, "## %s (%s)\n\n", version, date);
  return csemver_buffer_appendf(output, "## [%s] (%s)\n\n", version, date);
}

static int normalize_changelog_newlines(CsemverBuffer *output) {
  while (output->length > 0 && output->data[output->length - 1] == '\n')
    output->data[--output->length] = '\0';
  return output->length == 0 || csemver_buffer_append(output, "\n", 1);
}

static int regenerate_all_changelogs(
    const CsemverConfig *config, const char *version, const char *previous_tag,
    const char *new_tag, const Commit *commits, size_t commit_count,
    char tags[][SEMVER_TEXT_MAX], size_t tag_count, const char *date,
    const char *base, CsemverBuffer *output) {
  Commit *historical = calloc(COMMIT_MAX, sizeof(*historical));
  bool wrote_section = false;
  size_t i;
  if (historical == NULL)
    return 0;
  if (previous_tag == NULL || strcmp(previous_tag, new_tag) != 0) {
    if (!append_release_heading(output, base, version, previous_tag, new_tag,
                                date, false) ||
        !changelog_section(config, commits, commit_count, output))
      goto fail;
    wrote_section = true;
  }
  for (i = 0; i < tag_count; ++i) {
    char current_version[SEMVER_TEXT_MAX];
    char current_date[32];
    const char *older_tag = NULL;
    size_t j, historical_count = 0;
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
    if (!read_commits_range(config, older_tag, tags[i], historical,
                            &historical_count))
      goto fail;
    if (!get_tag_date(tags[i], current_date, sizeof current_date))
      snprintf(current_date, sizeof current_date, "%s", date);
    if (wrote_section && !csemver_buffer_append(output, "\n", 1))
      goto fail;
    if (!append_release_heading(output, base, current_version, older_tag,
                                tags[i], current_date, older_tag == NULL) ||
        !changelog_section(config, historical, historical_count, output))
      goto fail;
    wrote_section = true;
  }
  free(historical);
  return 1;
fail:
  free(historical);
  return 0;
}

static int render_changelog(const CsemverConfig *config, const char *version,
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
  struct tm local;
  size_t old_length = 0;
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
  }
  localtime_r(&now, &local);
  strftime(date, sizeof date, "%Y-%m-%d", &local);
  if (!config->dry_run && config->header[0] != '\0' &&
      (!csemver_buffer_append(output, config->header, strlen(config->header)) ||
       !csemver_buffer_append(output, "\n", 1)))
    goto fail;
  if (config->release_count == 0) {
    if ((!config->dry_run && !csemver_buffer_append(output, "\n", 1)) ||
        !regenerate_all_changelogs(config, version, previous_tag, new_tag,
                                   commits, commit_count, tags, tag_count, date,
                                   base, output) ||
        !normalize_changelog_newlines(output))
      goto fail;
    free(old_content);
    return 1;
  }
  if (!config->dry_run && !csemver_buffer_append(output, "\n", 1))
    goto fail;
  if (base[0] != '\0' && previous_tag != NULL) {
    if (!csemver_buffer_appendf(output, "## [%s](%s/compare/%s...%s) (%s)\n\n",
                                version, base, previous_tag, new_tag, date))
      goto fail;
  } else if (!csemver_buffer_appendf(output, "## [%s] (%s)\n\n", version, date))
    goto fail;
  if (!changelog_section(config, commits, commit_count, output))
    goto fail;
  if (*old_body != '\0' &&
      !csemver_buffer_append(output, old_body,
                             old_length - (size_t)(old_body - old_content)))
    goto fail;
  if (!normalize_changelog_newlines(output))
    goto fail;
  free(old_content);
  return 1;
fail:
  free(old_content);
  return 0;
}
