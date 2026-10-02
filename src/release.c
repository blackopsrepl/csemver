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
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef CSEMVER_VERSION
#define CSEMVER_VERSION "0.1.0"
#endif

#define ARG_MAX_COUNT 64
#define COMMIT_MAX 1024
#define ISSUE_REFERENCE_MAX 64
#define ISSUE_REFERENCE_TEXT_MAX 128

typedef struct {
  char hash[64];
  char subject[2048];
  char body[4096];
} Commit;

typedef struct {
  char text[ISSUE_REFERENCE_TEXT_MAX];
  bool closing;
} IssueReference;

typedef struct {
  const Commit *commit;
  const char *text;
  size_t text_length;
} BreakingNote;

typedef struct {
  int type_index;
  CsemverBuffer key;
} CommitSortKey;

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
  puts(
      "Usage: csemver [options]\n\n"
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
      "      --lerna-package NAME   Use package tags for bump selection\n"
      "      --packageFiles FILE... Override package version files\n"
      "      --bumpFiles FILE...    Override files to update\n"
      "      --issuePrefixes PFX... Issue prefixes to link\n"
      "      --release-count N      Changelog sections (0 all, N latest)\n"
      "      --preset NAME          Select conventional or Angular changelog\n"
      "      --scripts.EVENT CMD   Override a lifecycle script\n"
      "      --npmPublishHint TXT Override the release publishing hint\n"
      "  -s, --sign                 Sign release commit and tag\n"
      "      --signoff              Add a DCO signoff\n"
      "  -m, --message FORMAT       Deprecated; use "
      "--releaseCommitMessageFormat\n"
      "      --releaseCommitMessageFormat FORMAT\n"
      "      --header TEXT          Set changelog heading\n"
      "      --commitUrlFormat URL Set commit links ({{hash}})\n"
      "      --compareUrlFormat URL Set compare links ({{previousTag}}, "
      "{{currentTag}})\n"
      "      --issueUrlFormat URL  Set issue links ({{id}}, {{prefix}})\n"
      "      --userUrlFormat URL   Set user links ({{user}})\n"
      "      --preMajor            Apply pre-1.0.0 bump rules\n"
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
    if (strncmp(key, "--scripts.", sizeof "--scripts." - 1) == 0) {
      char error[256] = {0};
      const char *name = key + sizeof "--scripts." - 1;
      if (value == NULL && i + 1 < argc && argv[i + 1][0] != '-')
        value = argv[++i];
      if (value == NULL) {
        errorf("%s requires a command", key);
        return 2;
      }
      if (!csemver_config_set_script(config, name, value, error,
                                     sizeof error)) {
        errorf("%s", error);
        return 2;
      }
      continue;
    }
    if (strcmp(key, "--release-as") == 0 || strcmp(key, "--releaseAs") == 0 ||
        strcmp(key, "-r") == 0 || strcmp(key, "--infile") == 0 ||
        strcmp(key, "-i") == 0 || strcmp(key, "--tag-prefix") == 0 ||
        strcmp(key, "--tagPrefix") == 0 || strcmp(key, "-t") == 0 ||
        strcmp(key, "--path") == 0 || strcmp(key, "--preset") == 0 ||
        strcmp(key, "--message") == 0 || strcmp(key, "-m") == 0 ||
        strcmp(key, "--releaseCommitMessageFormat") == 0 ||
        strcmp(key, "--release-commit-message-format") == 0 ||
        strcmp(key, "--header") == 0 || strcmp(key, "--changelogHeader") == 0 ||
        strcmp(key, "--changelog-header") == 0 ||
        strcmp(key, "--lerna-package") == 0 ||
        strcmp(key, "--lernaPackage") == 0 ||
        strcmp(key, "--npmPublishHint") == 0 ||
        strcmp(key, "--npm-publish-hint") == 0 ||
        strcmp(key, "--commitUrlFormat") == 0 ||
        strcmp(key, "--commit-url-format") == 0 ||
        strcmp(key, "--compareUrlFormat") == 0 ||
        strcmp(key, "--compare-url-format") == 0 ||
        strcmp(key, "--issueUrlFormat") == 0 ||
        strcmp(key, "--issue-url-format") == 0 ||
        strcmp(key, "--userUrlFormat") == 0 ||
        strcmp(key, "--user-url-format") == 0) {
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
      if (strcmp(key, "--release-commit-message-format") == 0)
        name = "release-commit-message-format";
      if (strcmp(key, "--header") == 0)
        name = "header";
      if (strcmp(key, "--changelogHeader") == 0 ||
          strcmp(key, "--changelog-header") == 0)
        name = "changelogHeader";
      if (strcmp(key, "--lerna-package") == 0)
        name = "lerna-package";
      if (strcmp(key, "--lernaPackage") == 0)
        name = "lernaPackage";
      if (strcmp(key, "--npmPublishHint") == 0)
        name = "npmPublishHint";
      if (!csemver_config_set_string(config, name, value, error,
                                     sizeof error)) {
        errorf("%s", error);
        return 2;
      }
      continue;
    }
    if (strcmp(key, "--types") == 0 || strcmp(key, "--packageFiles") == 0 ||
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
    if (strcmp(key, "--dry-run") == 0 || strcmp(key, "--dryRun") == 0 ||
        strcmp(key, "--first-release") == 0 ||
        strcmp(key, "--firstRelease") == 0 || strcmp(key, "-f") == 0 ||
        strcmp(key, "--sign") == 0 || strcmp(key, "-s") == 0 ||
        strcmp(key, "--signoff") == 0 || strcmp(key, "--no-verify") == 0 ||
        strcmp(key, "--noVerify") == 0 || strcmp(key, "-n") == 0 ||
        strcmp(key, "--commit-all") == 0 || strcmp(key, "--commitAll") == 0 ||
        strcmp(key, "-a") == 0 || strcmp(key, "--silent") == 0 ||
        strcmp(key, "--tag-force") == 0 || strcmp(key, "--tagForce") == 0 ||
        strcmp(key, "--git-tag-fallback") == 0 ||
        strcmp(key, "--gitTagFallback") == 0 ||
        strcmp(key, "--noBumpWhenEmptyChanges") == 0 ||
        strcmp(key, "--no-bump-when-empty-changes") == 0 ||
        strcmp(key, "--preMajor") == 0 || strcmp(key, "--pre-major") == 0 ||
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
    /* Upstream yargs accepts unknown flags and positional arguments. */
    continue;
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
  char latest_stable_version[SEMVER_TEXT_MAX] = "";
  bool found_version = false;
  bool found_stable = false;
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
        bool relevant_version = true;
        if (config->has_prerelease && config->prerelease_id[0] != '\0' &&
            parsed.has_prerelease) {
          const char *separator = strchr(parsed.prerelease, '.');
          size_t identifier_length =
              separator == NULL ? strlen(parsed.prerelease)
                                : (size_t)(separator - parsed.prerelease);
          relevant_version =
              identifier_length == strlen(config->prerelease_id) &&
              strncmp(parsed.prerelease, config->prerelease_id,
                      identifier_length) == 0;
        }
        if (*tag_count < COMMIT_MAX)
          snprintf(tags[(*tag_count)++], SEMVER_TEXT_MAX, "%s", line);
        if (relevant_version &&
            (!found_version || semver_compare(candidate, latest_version) > 0)) {
          snprintf(latest_version, SEMVER_TEXT_MAX, "%s", candidate);
          found_version = true;
        }
        if (!parsed.has_prerelease &&
            (!found_stable ||
             semver_compare(candidate, latest_stable_version) > 0)) {
          snprintf(latest_stable_version, sizeof latest_stable_version, "%s",
                   candidate);
          snprintf(latest_tag, SEMVER_TEXT_MAX, "%s", line);
          found_stable = true;
        }
      }
    }
    line = strtok_r(NULL, "\n", &save);
  }
  free(output);
  return 1;
}

static int collect_lerna_tag(const CsemverConfig *config, char *latest_tag,
                             size_t tag_size) {
  const char *args[] = {"log", "--decorate", "--no-color", "--date-order",
                        NULL};
  char *output = NULL;
  char *cursor;
  int status = 0;
  size_t package_length = strlen(config->lerna_package);
  latest_tag[0] = '\0';
  if (package_length == 0)
    return 1;
  if (!run_git(args, &output, &status) || status != 0) {
    free(output);
    return 0;
  }
  cursor = output;
  while ((cursor = strstr(cursor, "tag: ")) != NULL) {
    char *tag_start = cursor + 5;
    char *tag_end = strpbrk(tag_start, ",)");
    char version[SEMVER_TEXT_MAX];
    size_t tag_length, version_length;
    Semver parsed;
    if (tag_end == NULL)
      tag_end = tag_start + strlen(tag_start);
    tag_length = (size_t)(tag_end - tag_start);
    if (tag_length > package_length + 1 &&
        memcmp(tag_start, config->lerna_package, package_length) == 0 &&
        tag_start[package_length] == '@') {
      version_length = tag_length - package_length - 1;
      if (version_length < sizeof version) {
        memcpy(version, tag_start + package_length + 1, version_length);
        version[version_length] = '\0';
        if (semver_parse(version, &parsed) && !parsed.has_prerelease) {
          if (tag_length >= tag_size) {
            free(output);
            return 0;
          }
          memcpy(latest_tag, tag_start, tag_length);
          latest_tag[tag_length] = '\0';
          free(output);
          return 1;
        }
      }
    }
    cursor = tag_end;
  }
  free(output);
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

static int preset_is_angular(const CsemverConfig *config) {
  return strcmp(config->preset, "angular") == 0 ||
         strcmp(config->preset, "conventional-changelog-angular") == 0;
}

static int preset_is_supported(const CsemverConfig *config) {
  return preset_is_angular(config) ||
         strcmp(config->preset, "conventional-changelog-conventionalcommits") ==
             0;
}

static int body_has_breaking_note(const char *body) {
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

static int commit_is_breaking(const char *subject, const char *body) {
  const char *colon = strchr(subject, ':');
  if (colon != NULL && colon > subject && colon[-1] == '!')
    return 1;
  return body_has_breaking_note(body);
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

static int preset_commit_type(const CsemverConfig *config, const char *subject,
                              char *type, size_t size, char *scope,
                              size_t scope_size, const char **description) {
  if (preset_is_angular(config))
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
                                         bool reverted[COMMIT_MAX]) {
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
  if (preset_commit_type(config, commit->subject, type, size, scope, scope_size,
                         description))
    return 1;
  if (!preset_is_angular(config) || !angular_revert_target(commit, target) ||
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
        char token[ISSUE_REFERENCE_TEXT_MAX];
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
          if (*reference_count >= ISSUE_REFERENCE_MAX)
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
  IssueReference references[ISSUE_REFERENCE_MAX];
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
    if (!run_git(args, &output, &status) || status != 0 || output == NULL) {
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
  size_t order[COMMIT_MAX];
  size_t count = 0, i;
  if (commit_count > COMMIT_MAX)
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

static int changelog_section(const CsemverConfig *config, const Commit *commits,
                             size_t commit_count, CsemverBuffer *output) {
  CsemverBuffer groups[CSEMVER_MAX_TYPES];
  CsemverBuffer breaking;
  CommitSortKey sort_keys[COMMIT_MAX];
  BreakingNote *breaking_notes = NULL;
  size_t breaking_note_count = 0, breaking_note_capacity = 0;
  bool used[CSEMVER_MAX_TYPES] = {false};
  bool reverted[COMMIT_MAX] = {false};
  char base[1024];
  size_t i, group_count;
  size_t group_order[CSEMVER_MAX_TYPES];
  bool angular = preset_is_angular(config);
  CsemverConfig angular_config;
  if (commit_count > COMMIT_MAX)
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
          !body_has_breaking_note(commits[i].body) ||
          strlen(type) >= sizeof angular_config.commit_types[0].type ||
          type_index(config, type) >= 0 ||
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
    has_breaking_note = body_has_breaking_note(commits[i].body);
    if (!parsed)
      scope[0] = '\0';
    if (!collect_breaking_notes(&commits[i], &breaking_notes,
                                &breaking_note_count, &breaking_note_capacity))
      goto fail;
    if (parsed && !has_breaking_note &&
        commit_is_breaking(commits[i].subject, "") &&
        !add_breaking_note(&breaking_notes, &breaking_note_count,
                           &breaking_note_capacity, &commits[i], description,
                           strlen(description)))
      goto fail;
    if (!parsed)
      continue;
    index = type_index(config, type);
    if (index < 0 ||
        (config->commit_types[index].hidden &&
         !(angular && has_breaking_note)) ||
        config->commit_types[index].section[0] == '\0')
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
    for (i = 1; i < group_count; ++i) {
      size_t group_index = group_order[i];
      size_t j = i;
      while (j > 0 && strcmp(config->commit_types[group_order[j - 1]].section,
                             config->commit_types[group_index].section) > 0) {
        group_order[j] = group_order[j - 1];
        --j;
      }
      group_order[j] = group_index;
    }
  }
  for (i = 0; i < breaking_note_count; ++i)
    if (!append_breaking_note(&breaking, config, &breaking_notes[i], base))
      goto fail;
  for (i = 0; i < group_count; ++i)
    if (used[i] && !append_sorted_group(&groups[i], config, sort_keys, commits,
                                        commit_count, (int)i, base))
      goto fail;
  if (!angular && breaking.length > 0) {
    if (!csemver_buffer_appendf(output, "### ⚠ BREAKING CHANGES\n\n") ||
        !csemver_buffer_append(output, breaking.data, breaking.length))
      goto fail;
  }
  {
    bool wrote_section = !angular && breaking.length > 0;
    for (i = 0; i < group_count; ++i) {
      size_t index = angular ? group_order[i] : i;
      if (!used[index])
        continue;
      if ((wrote_section && !csemver_buffer_append(output, "\n", 1)) ||
          !csemver_buffer_appendf(output, "### %s\n\n",
                                  config->commit_types[index].section) ||
          !csemver_buffer_append(output, groups[index].data,
                                 groups[index].length))
        goto fail;
      wrote_section = true;
    }
    if (angular && breaking.length > 0) {
      if ((wrote_section && !csemver_buffer_append(output, "\n", 1)) ||
          !csemver_buffer_append(output, "### BREAKING CHANGES\n\n",
                                 sizeof "### BREAKING CHANGES\n\n" - 1) ||
          !csemver_buffer_append(output, breaking.data, breaking.length))
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

static int calculate_bump(const CsemverConfig *config, const Commit *commits,
                          size_t count, const Semver *current) {
  bool angular = preset_is_angular(config);
  bool pre_major = config->pre_major || (!angular && current->major == 0);
  int bump = angular && count > 0 ? 1 : 0;
  size_t i;
  for (i = 0; i < count; ++i) {
    char type[128], scope[256];
    const char *description;
    int parsed =
        preset_commit_type(config, commits[i].subject, type, sizeof type, scope,
                           sizeof scope, &description);
    int index;
    if (angular) {
      if (body_has_breaking_note(commits[i].body))
        return config->pre_major && current->major == 0 ? 2 : 3;
      if (parsed && strcmp(type, "feat") == 0)
        bump = bump < (current->major == 0 && config->pre_major ? 1 : 2)
                   ? (current->major == 0 && config->pre_major ? 1 : 2)
                   : bump;
      continue;
    }
    if (!parsed)
      continue;
    (void)description;
    if (commit_is_breaking(commits[i].subject, commits[i].body))
      return pre_major ? 2 : 3;
    index = type_index(config, type);
    if (index < 0 || config->commit_types[index].hidden)
      continue;
    if (strcmp(type, "feat") == 0 || strcmp(type, "feature") == 0)
      bump = bump < (pre_major ? 1 : 2) ? (pre_major ? 1 : 2) : bump;
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
    Semver release_version;
    if (semver_parse(config->release_as, &release_version))
      return semver_format(&release_version, next, next_size);
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
  char lerna_tag[CSEMVER_VALUE_MAX];
  char current[SEMVER_TEXT_MAX], next[SEMVER_TEXT_MAX];
  char new_tag[SEMVER_TEXT_MAX];
  char message[CSEMVER_VALUE_MAX];
  bool is_private = false;
  bool lerna_bump = false;
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
  if (!preset_is_supported(&config)) {
    errorf("unsupported changelog preset '%s'", config.preset);
    return 2;
  }
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
  if (config.lerna_package[0] != '\0' && config.release_as[0] == '\0' &&
      !config.skip_bump && !config.first_release) {
    if (!collect_lerna_tag(&config, lerna_tag, sizeof lerna_tag)) {
      errorf("cannot inspect package release tags");
      return 1;
    }
    lerna_bump = true;
  }
  if (!semver_parse(current, &current_semver)) {
    errorf("invalid current version '%s'", current);
    return 1;
  }
  commits = calloc(COMMIT_MAX, sizeof(*commits));
  if (commits == NULL) {
    errorf("out of memory");
    return 1;
  }
  if (!read_commits(&config,
                    lerna_bump ? (lerna_tag[0] == '\0' ? NULL : lerna_tag)
                               : (latest_tag[0] == '\0' ? NULL : latest_tag),
                    commits, &commit_count)) {
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
  if ((!config.dry_run || lerna_bump) &&
      !read_commits(&config, latest_tag[0] == '\0' ? NULL : latest_tag, commits,
                    &commit_count)) {
    free(commits);
    errorf(config.dry_run ? "cannot read Git history"
                          : "cannot reread Git history after release hooks");
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

static const char *release_heading_level(const CsemverConfig *config,
                                         const char *version) {
  Semver parsed;
  if (!preset_is_angular(config))
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
  if (preset_is_angular(config))
    return csemver_buffer_appendf(output, "%s %s (%s)\n\n", level, version,
                                  date);
  return csemver_buffer_appendf(output, "## [%s](///compare/%s...%s) (%s)\n\n",
                                version, previous_tag, tag, date);
}

static int append_release_heading(const CsemverConfig *config,
                                  CsemverBuffer *output, const char *base,
                                  const char *version, const char *previous_tag,
                                  const char *tag, const char *date,
                                  bool initial_release) {
  if (previous_tag != NULL)
    return append_compare_heading(config, output, base, version, previous_tag,
                                  tag, date);
  if (preset_is_angular(config))
    return csemver_buffer_appendf(output, "%s %s (%s)\n\n",
                                  release_heading_level(config, version),
                                  version, date);
  if (initial_release)
    return csemver_buffer_appendf(output, "## %s (%s)\n", version, date);
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
    char tags[][SEMVER_TEXT_MAX], size_t tag_count, size_t history_limit,
    const char *date, const char *base, CsemverBuffer *output) {
  Commit *historical = calloc(COMMIT_MAX, sizeof(*historical));
  bool wrote_section = false;
  size_t i, history_count = 0;
  if (historical == NULL)
    return 0;
  if (previous_tag == NULL || strcmp(previous_tag, new_tag) != 0) {
    if (!append_release_heading(config, output, base, version, previous_tag,
                                new_tag, date, false) ||
        !changelog_section(config, commits, commit_count, output))
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
    if (!read_commits_range(config, older_tag, tags[i], historical,
                            &historical_count))
      goto fail;
    if (!get_tag_date(tags[i], current_date, sizeof current_date))
      snprintf(current_date, sizeof current_date, "%s", date);
    if (wrote_section &&
        !(output->length >= 2 && output->data[output->length - 1] == '\n' &&
          output->data[output->length - 2] == '\n') &&
        !csemver_buffer_append(output, "\n", 1))
      goto fail;
    if (!append_release_heading(config, output, base, current_version,
                                older_tag, tags[i], current_date,
                                older_tag == NULL) ||
        !changelog_section(config, historical, historical_count, output))
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
  struct tm utc;
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
  gmtime_r(&now, &utc);
  strftime(date, sizeof date, "%Y-%m-%d", &utc);
  if (!config->dry_run && ((config->header[0] != '\0' &&
                            !csemver_buffer_append(output, config->header,
                                                   strlen(config->header))) ||
                           !csemver_buffer_append(output, "\n", 1)))
    goto fail;
  if (config->release_count == 0) {
    if ((!config->dry_run && !csemver_buffer_append(output, "\n", 1)) ||
        !regenerate_all_changelogs(config, version, previous_tag, new_tag,
                                   commits, commit_count, tags, tag_count, 0,
                                   date, base, output) ||
        !normalize_changelog_newlines(output))
      goto fail;
    free(old_content);
    return 1;
  }
  if (config->release_count > 1) {
    size_t history_limit = (size_t)config->release_count - 1;
    if (!regenerate_all_changelogs(config, version, previous_tag, new_tag,
                                   commits, commit_count, tags, tag_count,
                                   history_limit, date, base, output))
      goto fail;
    if (*old_body != '\0' &&
        (!csemver_buffer_append(output, "\n", 1) ||
         !csemver_buffer_append(output, old_body,
                                old_length - (size_t)(old_body - old_content))))
      goto fail;
    if (!normalize_changelog_newlines(output))
      goto fail;
    free(old_content);
    return 1;
  }
  if (previous_tag != NULL) {
    if (!append_compare_heading(config, output, base, version, previous_tag,
                                new_tag, date))
      goto fail;
  } else if (config->first_release) {
    if (!append_release_heading(config, output, base, version, NULL, new_tag,
                                date, true))
      goto fail;
  } else if (!append_release_heading(config, output, base, version, NULL,
                                     new_tag, date, false))
    goto fail;
  if (!changelog_section(config, commits, commit_count, output))
    goto fail;
  if (*old_body != '\0' &&
      (!csemver_buffer_append(output, "\n", 1) ||
       !csemver_buffer_append(output, old_body,
                              old_length - (size_t)(old_body - old_content))))
    goto fail;
  if (!normalize_changelog_newlines(output))
    goto fail;
  free(old_content);
  return 1;
fail:
  free(old_content);
  return 0;
}
