#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "release.h"

#include "changelog.h"
#include "common.h"
#include "config.h"
#include "gitignore.h"
#include "lifecycle.h"
#include "tty.h"
#include "semver.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef FNM_EXTMATCH
#define IGNORE_FNM_FLAGS (FNM_PERIOD | FNM_EXTMATCH)
#else
#define IGNORE_FNM_FLAGS FNM_PERIOD
#endif

#ifndef CSEMVER_VERSION
#define CSEMVER_VERSION "0.1.0"
#endif

#define ARG_MAX_COUNT 64
#define CSEMVER_COMMIT_MAX 1024
#define ISSUE_REFERENCE_MAX 64
#define ISSUE_REFERENCE_TEXT_MAX 128
#define LIFECYCLE_SCRIPT_MAX_BUFFER (1024U * 1024U)
/* Truncated pipe flush applied when a lifecycle script exceeds its buffer. */
#define LIFECYCLE_PIPE_ERROR_FLUSH_LIMIT (64U * 1024U)
#define PACKAGE_UNSUPPORTED_FILENAME "__unsupported_filename"

static int uses_plain_text_updater(const CsemverFile *file);

static char *trim(char *text) {
  size_t length;
  while (*text != '\0' && isspace((unsigned char)*text))
    ++text;
  length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';
  return text;
}

static void errorf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fputs("csemver: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}


static int path_has_extension(const char *path) {
  const char *basename = strrchr(path, '/');
  const char *dot;
  const char *cursor;
  if (basename != NULL)
    ++basename;
  else
    basename = path;
  dot = strrchr(basename, '.');
  if (dot == NULL || dot == basename)
    return 0;
  for (cursor = basename; *cursor != '\0'; ++cursor)
    if (*cursor != '.')
      return 1;
  return 0;
}

static const char *compatibility_package_version(void) {
  static char version[CSEMVER_VALUE_MAX];
  char directory[CSEMVER_PATH_MAX];
  char package_path[CSEMVER_PATH_MAX + sizeof "/package.json"];
  if (getcwd(directory, sizeof directory) == NULL)
    return "unknown";
  if (path_has_extension(directory)) {
    char *slash = strrchr(directory, '/');
    if (slash == NULL)
      return "unknown";
    if (slash == directory)
      directory[1] = '\0';
    else
      *slash = '\0';
  }
  for (;;) {
    struct stat info;
    const char *suffix =
        strcmp(directory, "/") == 0 ? "package.json" : "/package.json";
    int path_length =
        snprintf(package_path, sizeof package_path, "%s%s", directory, suffix);
    if (path_length < 0 || (size_t)path_length >= sizeof package_path)
      return "unknown";
    if (stat(package_path, &info) == 0) {
      char *contents = NULL;
      char parse_error[256];
      int found;
      if (!csemver_read_file(package_path, &contents, NULL))
        return "unknown";
      found = csemver_version_read_text(package_path, "json", contents, version,
                                        sizeof version, NULL, parse_error,
                                        sizeof parse_error);
      free(contents);
      if (!found || version[0] == '\0')
        return "unknown";
      return version;
    }
    if (errno != ENOENT && errno != ENOTDIR)
      return "unknown";
    if (strcmp(directory, "/") == 0)
      break;
    {
      char *slash = strrchr(directory, '/');
      if (slash == NULL)
        break;
      if (slash == directory)
        directory[1] = '\0';
      else
        *slash = '\0';
    }
  }
  return "unknown";
}

// clang-format off
static void print_help(const char *program_path) {
  const char *program_name = "commit-and-tag-version";
  if (program_path != NULL && program_path[0] != '\0') {
    const char *slash = strrchr(program_path, '/');
    program_name = slash == NULL ? program_path : slash + 1;
  }
  printf("Usage: %s [options]\n\n", program_name);
  fputs(
      "Preset Configuration:\n"
      "      --header                      A string to be used as the main header section of the\n"
      "                                    CHANGELOG.                    [string] [default: \"# Changelog\n"
      "\n"
      "                         All notable changes to this project will be documented in this file. See\n"
      "  [commit-and-tag-version](https://github.com/absolute-version/commit-and-tag-version) for commit\n"
      "                                                                                      guidelines.\n"
      "                                                                                               \"]\n"
      "      --types                       An array of `type` objects representing the explicitly\n"
      "                                    supported commit message types, and whether they should show\n"
      "                                    up in generated `CHANGELOG`s.\n"
      "  [array] [default: [{\"type\":\"feat\",\"section\":\"Features\"},{\"type\":\"fix\",\"section\":\"Bug Fixes\"},{\"\n"
      "  type\":\"chore\",\"hidden\":true},{\"type\":\"docs\",\"hidden\":true},{\"type\":\"style\",\"hidden\":true},{\"typ\n"
      "       e\":\"refactor\",\"hidden\":true},{\"type\":\"perf\",\"hidden\":true},{\"type\":\"test\",\"hidden\":true}]]\n"
      "      --preMajor                    Boolean indicating whether or not the action being run\n"
      "                                    (generating CHANGELOG, recommendedBump, etc.) is being\n"
      "                                    performed for a pre-major release (<1.0.0).\n"
      "                                    This config setting will generally be set by tooling and not\n"
      "                                    a user.                            [boolean] [default: false]\n"
      "      --commitUrlFormat             A URL representing a specific commit at a hash.\n"
      "                          [string] [default: \"{{host}}/{{owner}}/{{repository}}/commit/{{hash}}\"]\n"
      "      --compareUrlFormat            A URL representing the comparison between two git SHAs.\n"
      "                                                                               [string] [default:\n"
      "                    \"{{host}}/{{owner}}/{{repository}}/compare/{{previousTag}}...{{currentTag}}\"]\n"
      "      --issueUrlFormat              A URL representing the issue format (allowing a different URL\n"
      "                                    format to be swapped in for Gitlab, Bitbucket, etc).\n"
      "                            [string] [default: \"{{host}}/{{owner}}/{{repository}}/issues/{{id}}\"]\n"
      ,
      stdout);
  fputs(
      "      --userUrlFormat               A URL representing the a user's profile URL on GitHub,\n"
      "                                    Gitlab, etc. This URL is used for substituting @bcoe with\n"
      "                                    https://github.com/bcoe in commit messages.\n"
      "                                                          [string] [default: \"{{host}}/{{user}}\"]\n"
      "      --releaseCommitMessageFormat  A string to be used to format the auto-generated release\n"
      "                                    commit message.\n"
      "                                             [string] [default: \"chore(release): {{currentTag}}\"]\n"
      "      --issuePrefixes               An array of prefixes used to detect references to issues\n"
      "                                                                         [array] [default: [\"#\"]]\n"
      "\n"
      "Options:\n"
      "  -h, --help              Show help                                                     [boolean]\n"
      "  -v, --version           Show version number                                           [boolean]\n"
      "      --packageFiles             [array] [default: [\"package.json\",\"bower.json\",\"manifest.json\"]]\n"
      "      --bumpFiles                                                               [array] [default:\n"
      "         [\"package.json\",\"bower.json\",\"manifest.json\",\"package-lock.json\",\"npm-shrinkwrap.json\"]]\n"
      "  -r, --release-as        Specify the release type manually (like npm version\n"
      "                          <major|minor|patch>)                                           [string]\n"
      "  -p, --prerelease        make a pre-release with optional option value to specify a tag id\n"
      "                                                                                         [string]\n"
      "  -i, --infile            Read the CHANGELOG from this file             [default: \"CHANGELOG.md\"]\n"
      "  -m, --message           [DEPRECATED] Commit message, replaces %s with new version.\n"
      "                          This option will be removed in the next major version, please use\n"
      "                          --releaseCommitMessageFormat.                                  [string]\n"
      "  -f, --first-release     Is this the first release?                   [boolean] [default: false]\n"
      "  -s, --sign              Should the git commit and tag be signed?     [boolean] [default: false]\n"
      "      --signoff           Should the git commit have a \"Signed-off-by\" trailer\n"
      "                                                                       [boolean] [default: false]\n"
      ,
      stdout);
  fputs(
      "  -n, --no-verify         Bypass pre-commit or commit-msg git hooks during the commit phase\n"
      "                                                                       [boolean] [default: false]\n"
      "  -a, --commit-all        Commit all staged changes, not just files affected by\n"
      "                          commit-and-tag-version                       [boolean] [default: false]\n"
      "      --silent            Don't print logs and errors                  [boolean] [default: false]\n"
      "  -t, --tag-prefix        Set a custom prefix for the git tag to be created\n"
      "                                                                          [string] [default: \"v\"]\n"
      "      --release-count     How many releases of changelog you want to generate. It counts from the\n"
      "                          upcoming release. Useful when you forgot to generate any previous\n"
      "                          changelog. Set to 0 to regenerate all.            [number] [default: 1]\n"
      "      --tag-force         Allow tag replacement                        [boolean] [default: false]\n"
      "      --scripts           Provide scripts to execute for lifecycle events (prebump, precommit,\n"
      "                          etc.,)                                                    [default: {}]\n"
      "      --skip              Map of steps in the release process that should be skipped[default: {}]\n"
      "      --dry-run           See the commands that running commit-and-tag-version would run\n"
      "                                                                       [boolean] [default: false]\n"
      "      --git-tag-fallback  fallback to git tags for version, if no meta-information file is found\n"
      "                          (e.g., package.json)                          [boolean] [default: true]\n"
      "      --path              Only populate commits made under this path                     [string]\n"
      "      --changelogHeader   [DEPRECATED] Use a custom header when generating and updating\n"
      "                          changelog.\n"
      "                          This option will be removed in the next major version, please use\n"
      "                          --header.                                                      [string]\n"
      "      --preset            Commit message guideline preset\n"
      "                       [string] [default: \"conventional-changelog-conventionalcommits\"]\n"
      "      --lerna-package     Name of the package from which the tags will be extracted      [string]\n"
      ,
      stdout);
  fputs(
      "      --npmPublishHint    Customized publishing hint                                     [string]\n"
      "\n"
      ,
      stdout);
  fputs("Examples:\n", stdout);
  printf("  %s%s\n", program_name, "                            Update changelog and tag release");
  printf("  %s -m \"%%s: see changelog for%s\n", program_name, "  Update changelog and tag release with custom");
  printf("  details\"%s\n", "                                          commit message");
}
// clang-format on

static int run_command(const char *const argv[], char **output, int *status) {
  return csemver_run_process(argv, output, status);
}

int csemver_run_git(const char *const args[], char **output, int *status) {
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

static int run_git_capture_streams(const char *const args[],
                                   char **stdout_output, char **stderr_output,
                                   int *status) {
  const char *argv[ARG_MAX_COUNT];
  size_t count = 0;
  int max_buffer_stream = 0;
  while (args[count] != NULL && count + 2 < ARG_MAX_COUNT) {
    argv[count + 1] = args[count];
    ++count;
  }
  argv[0] = "git";
  argv[count + 1] = NULL;
  if (count + 2 >= ARG_MAX_COUNT)
    return 0;
  if (!csemver_run_process_capture_streams(argv, stdout_output, stderr_output,
                                           LIFECYCLE_SCRIPT_MAX_BUFFER,
                                           &max_buffer_stream, status))
    return 0;
  if (max_buffer_stream != 0) {
    free(*stdout_output);
    free(*stderr_output);
    *stdout_output = NULL;
    *stderr_output = NULL;
    return 0;
  }
  return 1;
}

static int append_git_execfile_error(const char *const args[],
                                     const char *git_stderr,
                                     CsemverBuffer *message) {
  size_t index;
  if (!csemver_buffer_append(message, "Command failed: git",
                             sizeof("Command failed: git") - 1))
    return 0;
  for (index = 0; args[index] != NULL; ++index)
    if (!csemver_buffer_appendf(message, " %s", args[index]))
      return 0;
  return git_stderr[0] == '\0' ||
         (csemver_buffer_append(message, "\n", 1) &&
          csemver_buffer_append(message, git_stderr, strlen(git_stderr)));
}

static void print_execfile_error(const char *message) {
  fputs(message, stderr);
  fputc('\n', stderr);
}

static int run_git_execfile(const CsemverConfig *config,
                            const char *const args[], char **output,
                            int *status) {
  char *git_stdout = NULL;
  char *git_stderr = NULL;
  if (!run_git_capture_streams(args, &git_stdout, &git_stderr, status)) {
    free(git_stdout);
    free(git_stderr);
    return 0;
  }
  if (*status == 0) {
    /* Match upstream's console.warn newline for execFile stderr. */
    if (git_stderr[0] != '\0' && !config->silent) {
      fputs(git_stderr, stderr);
      fputc('\n', stderr);
    }
  } else {
    CsemverBuffer error_message;
    int error_message_valid;
    csemver_buffer_init(&error_message);
    error_message_valid =
        append_git_execfile_error(args, git_stderr, &error_message);
    if (!config->silent) {
      if (git_stderr[0] != '\0')
        print_execfile_error(git_stderr);
      else if (error_message_valid)
        print_execfile_error(error_message.data);
      if (error_message_valid)
        print_execfile_error(error_message.data);
    }
    csemver_buffer_free(&error_message);
  }
  free(git_stderr);
  if (output != NULL)
    *output = git_stdout;
  else
    free(git_stdout);
  return 1;
}

/* The configuration contract is csemver.toml, read from the current working
 * directory or selected with -c. */
static const char *find_default_config_path(char *storage,
                                            size_t storage_size) {
  char directory[CSEMVER_PATH_MAX];
  char candidate[CSEMVER_PATH_MAX];
  int length;

  if (getcwd(directory, sizeof directory) == NULL)
    return NULL;
  length = snprintf(candidate, sizeof candidate, "%s%s%s", directory,
                    strcmp(directory, "/") == 0 ? "" : "/", "csemver.toml");
  if (length < 0 || (size_t)length >= sizeof candidate ||
      access(candidate, F_OK) != 0)
    return NULL;
  if (snprintf(storage, storage_size, "%s", candidate) >= (int)storage_size)
    return NULL;
  return storage;
}

static int load_config(CsemverConfig *config, const char *path) {
  char *contents = NULL;
  char error[256] = {0};
  char discovered_path[CSEMVER_PATH_MAX];
  const char *selected_path = path;
  if (selected_path == NULL) {
    selected_path =
        find_default_config_path(discovered_path, sizeof discovered_path);
    if (selected_path == NULL)
      return 1;
  }
  if (!csemver_read_file(selected_path, &contents, NULL)) {
    errorf("cannot read config '%s'", selected_path);
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

static int package_path_ends_with(const char *path, const char *suffix) {
  size_t path_length = strlen(path);
  size_t suffix_length = strlen(suffix);
  return path_length >= suffix_length &&
         strcmp(path + path_length - suffix_length, suffix) == 0;
}

static void print_json_quoted(FILE *stream, const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  fputc('"', stream);
  while (*cursor != '\0') {
    switch (*cursor) {
    case '"':
      fputs("\\\"", stream);
      break;
    case '\\':
      fputs("\\\\", stream);
      break;
    case '\b':
      fputs("\\b", stream);
      break;
    case '\f':
      fputs("\\f", stream);
      break;
    case '\n':
      fputs("\\n", stream);
      break;
    case '\r':
      fputs("\\r", stream);
      break;
    case '\t':
      fputs("\\t", stream);
      break;
    default:
      if (*cursor < 0x20)
        fprintf(stream, "\\u%04x", (unsigned)*cursor);
      else
        fputc(*cursor, stream);
      break;
    }
    ++cursor;
  }
  fputc('"', stream);
}

static void warn_unsupported_package_bump_file(const char *filename) {
  fputs("Unable to obtain updater for: ", stderr);
  print_json_quoted(stderr, filename);
  fprintf(stderr,
          "\n - Error: Unsupported file (%s) provided for bumping.\n"
          " Please specify the updater `type` or use a custom `updater`.\n"
          " - Skipping...\n",
          filename);
}

static void warn_unsupported_package_bump_file_object(
    const char *filename, bool argument_json_valid, const char *argument_json) {
  fputs("Unable to obtain updater for: ", stderr);
  if (argument_json_valid && argument_json != NULL) {
    fputs(argument_json, stderr);
  } else {
    fputs("{\"filename\":", stderr);
    print_json_quoted(stderr, filename);
    fputc('}', stderr);
  }
  fprintf(stderr,
          "\n - Error: Unsupported file (%s) provided for bumping.\n"
          " Please specify the updater `type` or use a custom `updater`.\n"
          " - Skipping...\n",
          filename);
}

static void warn_unsupported_package_updater_type(const char *filename,
                                                  const char *type,
                                                  bool type_precedes_filename,
                                                  bool argument_json_valid,
                                                  const char *argument_json) {
  fputs("Unable to obtain updater for: ", stderr);
  if (argument_json_valid && argument_json != NULL) {
    fputs(argument_json, stderr);
  } else {
    fputc('{', stderr);
    if (type_precedes_filename) {
      fputs("\"type\":", stderr);
      print_json_quoted(stderr, type);
      fputs(",\"filename\":", stderr);
      print_json_quoted(stderr, filename);
    } else {
      fputs("\"filename\":", stderr);
      print_json_quoted(stderr, filename);
      fputs(",\"type\":", stderr);
      print_json_quoted(stderr, type);
    }
    fputc('}', stderr);
  }
  fprintf(stderr,
          "\n - Error: Unable to locate updater for provided type (%s).\n"
          " - Skipping...\n",
          type);
}

static int set_negated_boolean_option(CsemverConfig *config, const char *key) {
  char name[128];
  const char *start;
  size_t length;
  int camel_case = 0;

  if (strncmp(key, "--no-", 5) == 0) {
    start = key + 5;
  } else if (strncmp(key, "--no", 4) == 0 && key[4] >= 'A' && key[4] <= 'Z') {
    start = key + 4;
    camel_case = 1;
  } else {
    return 0;
  }
  length = strlen(start);
  if (length == 0 || length >= sizeof name)
    return 0;
  memcpy(name, start, length + 1);
  if (camel_case)
    name[0] = (char)(name[0] - 'A' + 'a');
  return csemver_config_set_bool(config, name, false, NULL, 0);
}

static int parse_args(int argc, char **argv, CsemverConfig *config,
                      const char **config_path) {
  const char *cli_package_files[CSEMVER_MAX_FILES];
  const char *cli_bump_files[CSEMVER_MAX_FILES];
  const char *cli_issue_prefixes[CSEMVER_MAX_PREFIXES];
  size_t cli_package_file_count = 0;
  size_t cli_bump_file_count = 0;
  size_t cli_issue_prefix_count = 0;
  bool cli_package_files_set = false;
  bool cli_bump_files_set = false;
  bool cli_issue_prefixes_set = false;
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
      print_help(argv[0]);
      return 1;
    }
    if (strcmp(key, "--version") == 0 || strcmp(key, "-v") == 0) {
      const char *program_name = argv[0];
      const char *version = CSEMVER_VERSION;
      const char *slash = strrchr(program_name, '/');
      if (slash != NULL)
        program_name = slash + 1;
      if (strcmp(program_name, "commit-and-tag-version") == 0)
        version = compatibility_package_version();
      puts(version);
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
      if (strcmp(key, "--packageFiles") == 0 ||
          strcmp(key, "--package-files") == 0) {
        if (count > CSEMVER_MAX_FILES - cli_package_file_count) {
          errorf("too many values for %s", key);
          return 2;
        }
        memcpy(cli_package_files + cli_package_file_count, values,
               count * sizeof values[0]);
        cli_package_file_count += count;
        cli_package_files_set = true;
      } else if (strcmp(key, "--bumpFiles") == 0 ||
                 strcmp(key, "--bump-files") == 0) {
        if (count > CSEMVER_MAX_FILES - cli_bump_file_count) {
          errorf("too many values for %s", key);
          return 2;
        }
        memcpy(cli_bump_files + cli_bump_file_count, values,
               count * sizeof values[0]);
        cli_bump_file_count += count;
        cli_bump_files_set = true;
      } else if (strcmp(key, "--issuePrefixes") == 0 ||
                 strcmp(key, "--issue-prefixes") == 0) {
        if (count > CSEMVER_MAX_PREFIXES - cli_issue_prefix_count) {
          errorf("too many issue prefixes");
          return 2;
        }
        memcpy(cli_issue_prefixes + cli_issue_prefix_count, values,
               count * sizeof values[0]);
        cli_issue_prefix_count += count;
        cli_issue_prefixes_set = true;
      } else if (!csemver_config_set_array(config, name, values, count, error,
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
      const char *name;
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
    if (set_negated_boolean_option(config, key))
      continue;
    /* Upstream yargs accepts unknown flags and positional arguments. */
    continue;
  }
  if (cli_package_files_set) {
    char error[256] = {0};
    if (!csemver_config_set_array(config, "packageFiles", cli_package_files,
                                  cli_package_file_count, error,
                                  sizeof error)) {
      errorf("%s", error);
      return 2;
    }
  }
  if (cli_bump_files_set) {
    char error[256] = {0};
    if (!csemver_config_set_array(config, "bumpFiles", cli_bump_files,
                                  cli_bump_file_count, error, sizeof error)) {
      errorf("%s", error);
      return 2;
    }
  }
  if (cli_issue_prefixes_set) {
    char error[256] = {0};
    if (!csemver_config_set_array(config, "issuePrefixes", cli_issue_prefixes,
                                  cli_issue_prefix_count, error,
                                  sizeof error)) {
      errorf("%s", error);
      return 2;
    }
  }
  if (config->has_changelog_header && config->changelog_header[0] != '\0')
    snprintf(config->header, sizeof config->header, "%s",
             config->changelog_header);
  if (!config->silent && config->has_message && config->message[0] != '\0')
    fprintf(stderr,
            "[commit-and-tag-version]: --message (-m) will be removed in the "
            "next major release. Use --releaseCommitMessageFormat.\n");
  if (!config->silent && config->has_changelog_header &&
      config->changelog_header[0] != '\0')
    fprintf(stderr,
            "[commit-and-tag-version]: --changelogHeader will be removed in "
            "the next major release. Use --header.\n");
  return 0;
}

/* RELEASE_ENGINE */

static char *find_git_tag_marker(char *cursor) {
  while (*cursor != '\0') {
    if ((*cursor == 't' || *cursor == 'T') &&
        (cursor[1] == 'a' || cursor[1] == 'A') &&
        (cursor[2] == 'g' || cursor[2] == 'G') && cursor[3] == ':')
      return cursor;
    ++cursor;
  }
  return NULL;
}

static int collect_tags(const CsemverConfig *config,
                        char tags[][SEMVER_TEXT_MAX], size_t *tag_count,
                        char *latest_version, char *latest_tag) {
  const char *args[] = {"log", "--decorate", "--no-color", "--date-order",
                        NULL};
  char *output = NULL;
  char *cursor;
  int status = 0;
  char latest_stable_version[SEMVER_TEXT_MAX] = "";
  size_t prefix_length = strlen(config->tag_prefix);
  bool found_version = false;
  bool found_stable = false;
  *tag_count = 0;
  latest_version[0] = latest_tag[0] = '\0';
  if (!csemver_run_git(args, &output, &status) || status != 0) {
    free(output);
    return 0;
  }
  cursor = output;
  while ((cursor = find_git_tag_marker(cursor)) != NULL) {
    char *start = cursor + 4;
    char *end;
    size_t tag_length;
    char tag[SEMVER_TEXT_MAX];
    char candidate[SEMVER_TEXT_MAX];
    Semver parsed;

    while (*start != '\0' && isspace((unsigned char)*start))
      ++start;
    end = start;
    while (*end != '\0' && *end != ',' && *end != ')' && *end != '\n' &&
           *end != '\r')
      ++end;
    cursor = *end == '\0' ? end : end + 1;
    if ((*end != ',' && *end != ')') || end == start)
      continue;
    tag_length = (size_t)(end - start);
    if (tag_length >= sizeof tag)
      continue;
    memcpy(tag, start, tag_length);
    tag[tag_length] = '\0';
    if (strncmp(tag, config->tag_prefix, prefix_length) != 0 ||
        tag_length - prefix_length >= sizeof candidate)
      continue;
    snprintf(candidate, sizeof candidate, "%s", tag + prefix_length);
    if (semver_parse(candidate, &parsed)) {
      bool relevant_version = true;
      if (config->has_prerelease && config->prerelease_id[0] != '\0' &&
          parsed.has_prerelease) {
        const char *separator = strchr(parsed.prerelease, '.');
        size_t identifier_length =
            separator == NULL ? strlen(parsed.prerelease)
                              : (size_t)(separator - parsed.prerelease);
        relevant_version = identifier_length == strlen(config->prerelease_id) &&
                           strncmp(parsed.prerelease, config->prerelease_id,
                                   identifier_length) == 0;
      }
      if (*tag_count < CSEMVER_COMMIT_MAX)
        snprintf(tags[(*tag_count)++], SEMVER_TEXT_MAX, "%s", tag);
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
        snprintf(latest_tag, SEMVER_TEXT_MAX, "%s", tag);
        found_stable = true;
      }
    }
  }
  free(output);
  return 1;
}

typedef enum {
  FALLBACK_TAG_NO_TAGS,
  FALLBACK_TAG_SELECTED,
  FALLBACK_TAG_NULL_VERSION,
  FALLBACK_TAG_UNDEFINED_VERSION,
  FALLBACK_TAG_INVALID_REGEX
} FallbackTagStatus;

static FallbackTagStatus
select_fallback_tag_version(const CsemverConfig *config,
                            char tags[][SEMVER_TEXT_MAX], size_t tag_count,
                            char *version, size_t version_size) {
  size_t prefix_length = strlen(config->tag_prefix);
  char *pattern;
  regex_t prefix_regex;
  bool filter_prerelease =
      config->has_prerelease && config->prerelease_id[0] != '\0';
  bool found = false;
  bool invalid_version = false;
  size_t eligible_count = 0;
  char best_version[SEMVER_TEXT_MAX] = "";

  if (tag_count == 0) {
    snprintf(version, version_size, "1.0.0");
    return FALLBACK_TAG_NO_TAGS;
  }
  if (prefix_length > SIZE_MAX - 2)
    return FALLBACK_TAG_INVALID_REGEX;
  pattern = malloc(prefix_length + 2);
  if (pattern == NULL)
    return FALLBACK_TAG_INVALID_REGEX;
  pattern[0] = '^';
  memcpy(pattern + 1, config->tag_prefix, prefix_length + 1);
  if (regcomp(&prefix_regex, pattern, REG_EXTENDED) != 0) {
    free(pattern);
    return FALLBACK_TAG_INVALID_REGEX;
  }
  free(pattern);

  for (size_t i = 0; i < tag_count; ++i) {
    const char *candidate = tags[i];
    regmatch_t match;
    int regex_status = regexec(&prefix_regex, tags[i], 1, &match, 0);
    Semver parsed;

    if (regex_status == 0) {
      if (match.rm_so != 0 || match.rm_eo < match.rm_so) {
        regfree(&prefix_regex);
        return FALLBACK_TAG_INVALID_REGEX;
      }
      candidate += (size_t)match.rm_eo;
    } else if (regex_status != REG_NOMATCH) {
      regfree(&prefix_regex);
      return FALLBACK_TAG_INVALID_REGEX;
    }

    if (filter_prerelease) {
      if (!semver_parse(candidate, &parsed))
        continue;
      if (parsed.has_prerelease) {
        const char *separator = strchr(parsed.prerelease, '.');
        size_t identifier_length =
            separator == NULL ? strlen(parsed.prerelease)
                              : (size_t)(separator - parsed.prerelease);
        if (identifier_length != strlen(config->prerelease_id) ||
            strncmp(parsed.prerelease, config->prerelease_id,
                    identifier_length) != 0)
          continue;
      }
    }

    ++eligible_count;
    if (!semver_clean(candidate, &parsed)) {
      invalid_version = true;
      continue;
    }
    char cleaned[SEMVER_TEXT_MAX];
    if (!semver_format(&parsed, cleaned, sizeof cleaned)) {
      regfree(&prefix_regex);
      return FALLBACK_TAG_NULL_VERSION;
    }
    if (!found || semver_compare(cleaned, best_version) > 0) {
      snprintf(best_version, sizeof best_version, "%s", cleaned);
      found = true;
    }
  }
  regfree(&prefix_regex);

  if (filter_prerelease && eligible_count == 0)
    return FALLBACK_TAG_UNDEFINED_VERSION;
  if (invalid_version || !found)
    return FALLBACK_TAG_NULL_VERSION;
  snprintf(version, version_size, "%s", best_version);
  return FALLBACK_TAG_SELECTED;
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
  if (!csemver_run_git(args, &output, &status) || status != 0) {
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
    if (config->package_files[index].compatibility_unsupported_type) {
      warn_unsupported_package_updater_type(
          config->package_files[index].filename,
          config->package_files[index].type,
          config->package_files[index]
              .compatibility_updater_type_precedes_filename,
          config->package_files[index]
              .compatibility_updater_argument_json_valid,
          config->package_files[index].compatibility_updater_argument_json);
      return -1;
    }
    if (config->package_files[index].compatibility_unsupported_filename) {
      if (config->package_files[index].compatibility_updater_argument_object)
        warn_unsupported_package_bump_file_object(
            config->package_files[index].filename,
            config->package_files[index]
                .compatibility_updater_argument_json_valid,
            config->package_files[index].compatibility_updater_argument_json);
      else
        warn_unsupported_package_bump_file(
            config->package_files[index].filename);
      return -1;
    }
    if (!csemver_read_file(config->package_files[index].filename, &content,
                           NULL))
      continue;
    if ((config->package_files[index].has_version_pattern
             ? csemver_version_read_pattern_text(
                   content, config->package_files[index].version_pattern,
                   config->package_files[index].version_group, version,
                   SEMVER_TEXT_MAX, error, sizeof error)
             : csemver_version_read_text(config->package_files[index].filename,
                                         config->package_files[index].type,
                                         content, version, SEMVER_TEXT_MAX,
                                         is_private, error, sizeof error))) {
      free(content);
      return 1;
    }
    free(content);
  }
  return 0;
}

int csemver_read_commits_range(const CsemverConfig *config,
                              const char *previous_tag, const char *end_ref,
                              Commit *commits, size_t *commit_count);

static int read_commits(const CsemverConfig *config, const char *previous_tag,
                        Commit *commits, size_t *commit_count) {
  return csemver_read_commits_range(config, previous_tag, "HEAD", commits,
                            commit_count);
}

int csemver_read_commits_range(const CsemverConfig *config,
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
  if (!csemver_run_git(args, &output, &status) || status != 0) {
    free(output);
    return 0;
  }
  cursor = output;
  while (*cursor != '\0' && *commit_count < CSEMVER_COMMIT_MAX) {
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

static int calculate_bump(const CsemverConfig *config, const Commit *commits,
                          size_t count, const Semver *current) {
  bool angular = csemver_preset_is_angular(config);
  bool pre_major = config->pre_major || (!angular && current->major == 0);
  int bump = angular && count > 0 ? 1 : 0;
  size_t i;
  for (i = 0; i < count; ++i) {
    char type[128], scope[256];
    const char *description;
    int parsed =
        csemver_preset_commit_type(config, commits[i].subject, type, sizeof type, scope,
                           sizeof scope, &description);
    int index;
    if (angular) {
      if (csemver_body_has_breaking_note(commits[i].body))
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
    if (csemver_commit_is_breaking(commits[i].subject, commits[i].body))
      return pre_major ? 2 : 3;
    index = csemver_type_index(config, type);
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


static int prepare_bump(CsemverConfig *config) {
  char *output = NULL;
  char *text;
  char version[SEMVER_TEXT_MAX];
  size_t used = 0;
  Semver parsed;
  if (config->skip_bump)
    return 1;
  if (!csemver_run_lifecycle(config, "prerelease") ||
      !csemver_run_lifecycle_capture(config, "prebump", &output)) {
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

static int parse_prerelease_number(const char *text, unsigned long *number) {
  const unsigned char *cursor = (const unsigned char *)text;
  char *end;
  unsigned long parsed;
  if (text == NULL || text[0] == '\0')
    return 0;
  for (; *cursor != '\0'; ++cursor)
    if (!isdigit(*cursor))
      return 0;
  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0')
    return 0;
  *number = parsed;
  return 1;
}

static void split_prerelease_tokens(const Semver *version, char *first,
                                    size_t first_size, char *second,
                                    size_t second_size) {
  char *dot;
  snprintf(first, first_size, "%s", version->prerelease);
  dot = strchr(first, '.');
  if (dot == NULL) {
    second[0] = '\0';
    return;
  }
  *dot++ = '\0';
  snprintf(second, second_size, "%s", dot);
  dot = strchr(second, '.');
  if (dot != NULL)
    *dot = '\0';
}

static unsigned long proposed_prerelease_number(const Semver *version,
                                                const char *identifier) {
  char first[SEMVER_IDENTIFIER_MAX], second[SEMVER_IDENTIFIER_MAX];
  unsigned long number;
  split_prerelease_tokens(version, first, sizeof first, second, sizeof second);
  if (identifier[0] == '\0')
    return parse_prerelease_number(first, &number) ? number : 0;
  return parse_prerelease_number(second, &number) ? number : 0;
}

static int tag_prerelease_number(const Semver *version, const char *identifier,
                                 unsigned long *number) {
  char first[SEMVER_IDENTIFIER_MAX], second[SEMVER_IDENTIFIER_MAX];
  split_prerelease_tokens(version, first, sizeof first, second, sizeof second);
  if (identifier[0] == '\0')
    return parse_prerelease_number(first, number);
  if (strcmp(first, identifier) != 0)
    return 0;
  if (!parse_prerelease_number(second, number))
    *number = 0;
  return 1;
}

static int resolve_unique_prerelease(const CsemverConfig *config,
                                     char tags[][SEMVER_TEXT_MAX],
                                     size_t tag_count, char *version_text,
                                     size_t version_size) {
  Semver proposed;
  unsigned long current_number, max_number = 0;
  size_t prefix_length = strlen(config->tag_prefix);
  bool found = false;
  if (!config->has_prerelease || !semver_parse(version_text, &proposed) ||
      !proposed.has_prerelease)
    return 1;
  current_number = proposed_prerelease_number(&proposed, config->prerelease_id);
  for (size_t i = 0; i < tag_count; ++i) {
    Semver tagged;
    unsigned long tagged_number;
    if (strncmp(tags[i], config->tag_prefix, prefix_length) != 0 ||
        !semver_parse(tags[i] + prefix_length, &tagged) ||
        tagged.major != proposed.major || tagged.minor != proposed.minor ||
        tagged.patch != proposed.patch || !tagged.has_prerelease ||
        !tag_prerelease_number(&tagged, config->prerelease_id, &tagged_number))
      continue;
    if (!found || tagged_number > max_number)
      max_number = tagged_number;
    found = true;
  }
  if (!found || current_number > max_number)
    return 1;
  if (max_number == ULONG_MAX)
    return 0;
  if (config->prerelease_id[0] == '\0') {
    if (snprintf(proposed.prerelease, sizeof proposed.prerelease, "%lu",
                 max_number + 1) >= (int)sizeof proposed.prerelease)
      return 0;
  } else if (snprintf(proposed.prerelease, sizeof proposed.prerelease, "%s.%lu",
                      config->prerelease_id,
                      max_number + 1) >= (int)sizeof proposed.prerelease) {
    return 0;
  }
  proposed.has_prerelease = 1;
  return semver_format(&proposed, version_text, version_size);
}

static int parse_release_as_version(const char *text, Semver *version) {
  char normalized[SEMVER_TEXT_MAX];
  const char *start = text;
  const char *end;
  size_t length;
  if (text == NULL)
    return 0;
  while (isspace((unsigned char)*start))
    ++start;
  end = start + strlen(start);
  while (end > start && isspace((unsigned char)end[-1]))
    --end;
  if (start < end && *start == 'v')
    ++start;
  length = (size_t)(end - start);
  if (length == 0 || length >= sizeof normalized)
    return 0;
  memcpy(normalized, start, length);
  normalized[length] = '\0';
  return semver_parse(normalized, version);
}

static int release_as_is_valid(const char *release_as) {
  Semver exact_version;
  char release_type[SEMVER_TEXT_MAX];
  size_t length = strlen(release_as);
  size_t index;
  if (length == 0 || parse_release_as_version(release_as, &exact_version))
    return 1;
  if (length >= sizeof release_type)
    return 0;
  for (index = 0; index < length; ++index)
    release_type[index] = (char)tolower((unsigned char)release_as[index]);
  release_type[length] = '\0';
  return strcmp(release_type, "major") == 0 ||
         strcmp(release_type, "minor") == 0 ||
         strcmp(release_type, "patch") == 0;
}

/* Upstream validates these types case-insensitively but later passes the
 * original spelling to semver.inc, which yields null for mixed-case values.
 */
static int release_as_type_case_mismatch(const char *release_as) {
  static const char *const release_types[] = {"major", "minor", "patch"};
  size_t type_index;
  size_t release_as_length = strlen(release_as);
  for (type_index = 0;
       type_index < sizeof release_types / sizeof release_types[0];
       ++type_index) {
    const char *release_type = release_types[type_index];
    size_t index;
    if (strlen(release_type) != release_as_length ||
        strcmp(release_type, release_as) == 0)
      continue;
    for (index = 0; index < release_as_length; ++index)
      if (release_type[index] !=
          (char)tolower((unsigned char)release_as[index]))
        break;
    if (index == release_as_length)
      return 1;
  }
  return 0;
}

static int validate_release_as_prerelease(const CsemverConfig *config) {
  Semver release_version;
  if (!config->has_prerelease || config->release_as[0] == '\0' ||
      !parse_release_as_version(config->release_as, &release_version))
    return 1;
  if (release_version.has_prerelease) {
    const char *last_separator = strrchr(release_version.prerelease, '.');
    size_t release_id_size =
        last_separator == NULL
            ? 0
            : (size_t)(last_separator - release_version.prerelease);
    size_t requested_id_size = strlen(config->prerelease_id);
    if (release_id_size != requested_id_size ||
        strncmp(release_version.prerelease, config->prerelease_id,
                release_id_size) != 0) {
      fputs(
          "releaseAs and prerelease have conflicting prerelease identifiers\n",
          stderr);
      return 0;
    }
  } else if (config->prerelease_id[0] == '\0') {
    fprintf(stderr, "Invalid Version: %lu.%lu.%lu-.0\n", release_version.major,
            release_version.minor, release_version.patch);
    return 0;
  }
  return 1;
}

static int generate_prerelease_version(const CsemverConfig *config,
                                       const Semver *version, int bump,
                                       char *next, size_t next_size) {
  char type[32];
  if (version->has_prerelease) {
    int active_priority = version->patch != 0   ? 0
                          : version->minor != 0 ? 1
                          : version->major != 0 ? 2
                                                : -1;
    int expected_priority = bump == 3 ? 2 : bump == 2 ? 1 : 0;
    if (active_priority >= expected_priority)
      return semver_bump(version, "prerelease", config->prerelease_id, next,
                         next_size);
  }
  if (snprintf(type, sizeof type, "pre%s", bump_name(bump)) >= (int)sizeof type)
    return 0;
  return semver_bump(version, type, config->prerelease_id, next, next_size);
}

static int generate_version(const CsemverConfig *config, const char *current,
                            int bump, char *next, size_t next_size) {
  Semver parsed;
  if (!semver_parse(current, &parsed))
    return 0;
  if (config->first_release && config->release_as[0] == '\0')
    return semver_format(&parsed, next, next_size);
  if (config->release_as[0] != '\0') {
    Semver release_version;
    if (parse_release_as_version(config->release_as, &release_version)) {
      if (config->has_prerelease && !release_version.has_prerelease) {
        if (snprintf(release_version.prerelease,
                     sizeof release_version.prerelease, "%s.0",
                     config->prerelease_id) >=
            (int)sizeof release_version.prerelease)
          return 0;
        release_version.has_prerelease = 1;
      }
      if (config->has_prerelease && parsed.has_prerelease &&
          parsed.major == release_version.major &&
          parsed.minor == release_version.minor &&
          parsed.patch == release_version.patch) {
        char release_text[SEMVER_TEXT_MAX];
        if (!semver_format(&release_version, release_text, sizeof release_text))
          return 0;
        if (semver_compare(release_text, current) <= 0) {
          char incremented_text[SEMVER_TEXT_MAX];
          Semver incremented;
          if (!semver_bump(&parsed, "prerelease", config->prerelease_id,
                           incremented_text, sizeof incremented_text) ||
              !semver_parse(incremented_text, &incremented))
            return 0;
          incremented.has_build = release_version.has_build;
          strcpy(incremented.build, release_version.build);
          return semver_format(&incremented, next, next_size);
        }
      }
      return semver_format(&release_version, next, next_size);
    }
    if (config->has_prerelease) {
      int release_bump = strcmp(config->release_as, "major") == 0   ? 3
                         : strcmp(config->release_as, "minor") == 0 ? 2
                         : strcmp(config->release_as, "patch") == 0 ? 1
                                                                    : 0;
      if (release_bump == 0)
        return 0;
      return generate_prerelease_version(config, &parsed, release_bump, next,
                                         next_size);
    }
    return semver_bump(&parsed, config->release_as, NULL, next, next_size);
  }
  if (bump == 0) {
    if (config->no_bump_when_empty_changes)
      return semver_format(&parsed, next, next_size);
    return 0;
  }
  if (config->has_prerelease)
    return generate_prerelease_version(config, &parsed, bump, next, next_size);
  return semver_bump(&parsed, bump_name(bump), NULL, next, next_size);
}

typedef enum {
  BUMP_FILE_MISSING,
  BUMP_FILE_REGULAR,
  BUMP_FILE_OTHER,
} BumpFileKind;

static BumpFileKind bump_file_kind(const char *filename) {
  struct stat status;
  if (lstat(filename, &status) != 0)
    return BUMP_FILE_MISSING;
  return S_ISREG(status.st_mode) ? BUMP_FILE_REGULAR : BUMP_FILE_OTHER;
}

static int update_files(const CsemverConfig *config, const char *version,
                        bool version_is_null,
                        char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                        size_t *path_count, bool dry_run) {
  size_t i;
  *path_count = 0;
  if (config->skip_bump)
    return 1;
  if (config->first_release)
    return dry_run ? 1 : csemver_run_lifecycle(config, "postbump");
  for (i = 0; i < config->bump_file_count; ++i) {
    char *content = NULL;
    char *updated = NULL;
    char old_version[SEMVER_TEXT_MAX];
    char error[256] = {0};
    size_t updated_size = 0;
    BumpFileKind file_kind;
    int update_ok;
    if (config->bump_files[i].compatibility_unsupported_type) {
      warn_unsupported_package_updater_type(
          config->bump_files[i].filename, config->bump_files[i].type,
          config->bump_files[i].compatibility_updater_type_precedes_filename,
          config->bump_files[i].compatibility_updater_argument_json_valid,
          config->bump_files[i].compatibility_updater_argument_json);
      continue;
    }
    if (strcmp(config->bump_files[i].type, PACKAGE_UNSUPPORTED_FILENAME) == 0) {
      warn_unsupported_package_bump_file(config->bump_files[i].filename);
      continue;
    }
    if (csemver_path_is_gitignored(config->bump_files[i].filename)) {
      printf("Not updating file '%s', as it is ignored in Git\n",
             config->bump_files[i].filename);
      continue;
    }
    file_kind = bump_file_kind(config->bump_files[i].filename);
    if (file_kind == BUMP_FILE_MISSING)
      continue;
    if (file_kind == BUMP_FILE_OTHER) {
      printf("Not updating '%s', as it is not a file\n",
             config->bump_files[i].filename);
      continue;
    }
    if (!csemver_read_file(config->bump_files[i].filename, &content, NULL))
      continue;
    update_ok =
        config->bump_files[i].has_version_pattern
            ? csemver_version_update_pattern_text(
                  content, config->bump_files[i].version_pattern,
                  config->bump_files[i].version_group,
                  version_is_null ? NULL : version, &updated, &updated_size,
                  old_version, sizeof old_version, error, sizeof error)
            : csemver_version_update_text(
                  config->bump_files[i].filename, config->bump_files[i].type,
                  content, version_is_null ? NULL : version, &updated,
                  &updated_size, old_version, sizeof old_version, error,
                  sizeof error);
    if (!update_ok) {
      fflush(stdout);
      fprintf(stderr, "%s\n", error);
      free(content);
      free(updated);
      continue;
    }
    if (!config->silent) {
      const char *display_old = uses_plain_text_updater(&config->bump_files[i])
                                    ? content
                                    : old_version;
      csemver_print_checkpoint_tick(config);
      printf(" bumping version in ");
      printf("%s", config->bump_files[i].filename);
      printf(" from ");
      csemver_print_bold(display_old);
      printf(" to ");
      csemver_print_bold(version);
      putchar('\n');
    }
    if (!dry_run && !csemver_write_file(config->bump_files[i].filename, updated,
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
  return dry_run ? 1 : csemver_run_lifecycle(config, "postbump");
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
  if (!csemver_run_lifecycle(config, "prechangelog"))
    return 0;
  if (!config->silent && access(config->infile, F_OK) != 0) {
    csemver_print_checkpoint_tick(config);
    printf(" created ");
    csemver_print_bold(config->infile);
    putchar('\n');
  }
  csemver_buffer_init(&content);
  if (!csemver_render_changelog(config, version, previous_tag, new_tag, commits,
                        commit_count, tags, tag_count, &content)) {
    csemver_buffer_free(&content);
    errorf("failed to generate changelog");
    return 0;
  }
  if (!config->silent) {
    csemver_print_checkpoint_tick(config);
    printf(" outputting changes to ");
    csemver_print_bold(config->infile);
    putchar('\n');
  }
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
  if (ok)
    ok = csemver_run_lifecycle(config, "postchangelog");
  return ok;
}

static void
print_commit_summary(const CsemverConfig *config,
                     char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                     size_t path_count);
static int
print_publish_hint(const CsemverConfig *config, bool is_private,
                   char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                   size_t path_count);

static size_t
append_release_paths(const CsemverConfig *config,
                     char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                     size_t path_count, const char *args[], size_t index) {
  bool changelog_is_last = !config->skip_changelog && path_count > 0 &&
                           strcmp(paths[path_count - 1], config->infile) == 0;
  size_t path_limit = path_count - (changelog_is_last ? 1 : 0);
  size_t path_index;
  if (changelog_is_last && index + 1 < ARG_MAX_COUNT)
    args[index++] = paths[path_count - 1];
  for (path_index = 0; path_index < path_limit && index + 1 < ARG_MAX_COUNT;
       ++path_index)
    args[index++] = paths[path_index];
  return index;
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
  if (!csemver_run_lifecycle_capture(config, "precommit", &hook_message)) {
    free(hook_message);
    return 0;
  }
  if (hook_message != NULL && hook_message[0] != '\0') {
    const char *format = hook_message;
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
  print_commit_summary(config, paths, path_count);
  if (path_count > 0) {
    args[index++] = "add";
    index = append_release_paths(config, paths, path_count, args, index);
    args[index] = NULL;
    if (!run_git_execfile(config, args, NULL, &status)) {
      errorf("git add failed");
      return 0;
    }
    if (status != 0)
      return 0;
    index = 0;
  } else if (config->commit_all) {
    args[index++] = "add";
    args[index] = NULL;
    if (!run_git_execfile(config, args, NULL, &status) || status != 0)
      return 0;
    index = 0;
  }
  args[index++] = "commit";
  if (config->no_verify)
    args[index++] = "--no-verify";
  if (config->sign)
    args[index++] = "-S";
  if (config->signoff)
    args[index++] = "--signoff";
  if (!config->commit_all)
    index = append_release_paths(config, paths, path_count, args, index);
  args[index++] = "-m";
  args[index++] = message;
  args[index] = NULL;
  if (!run_git_execfile(config, args, NULL, &status)) {
    errorf("git commit failed");
    return 0;
  }
  if (status != 0)
    return 0;
  return csemver_run_lifecycle(config, "postcommit");
}

static int tag_release(const CsemverConfig *config, const char *tag,
                       const char *message, bool is_private,
                       char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                       size_t path_count) {
  const char *args[8];
  size_t index = 0;
  int status = 0;
  if (config->skip_tag)
    return 1;
  if (!csemver_run_lifecycle(config, "pretag"))
    return 0;
  if (!config->silent) {
    csemver_print_checkpoint_tick(config);
    printf(" tagging release ");
    csemver_print_bold(config->tag_prefix);
    csemver_print_bold(tag + strlen(config->tag_prefix));
    putchar('\n');
  }
  args[index++] = "tag";
  if (config->sign)
    args[index++] = "-s";
  else
    args[index++] = "-a";
  if (config->tag_force)
    args[index++] = "-f";
  args[index++] = tag;
  args[index++] = "-m";
  args[index++] = message;
  args[index] = NULL;
  if (!run_git_execfile(config, args, NULL, &status)) {
    errorf("git tag failed for %s", tag);
    return 0;
  }
  if (status != 0)
    return 0;
  if (!print_publish_hint(config, is_private, paths, path_count))
    return 0;
  return csemver_run_lifecycle(config, "posttag");
}

static void
print_commit_summary(const CsemverConfig *config,
                     char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                     size_t path_count) {
  if (config->silent || config->skip_commit)
    return;
  csemver_print_checkpoint_tick(config);
  fputs(" committing ", stdout);
  bool has_changelog = !config->skip_changelog && path_count > 0 &&
                       strcmp(paths[path_count - 1], config->infile) == 0;
  size_t version_path_count = path_count - (has_changelog ? 1 : 0);
  for (size_t i = version_path_count; i > 0; --i) {
    if (i != version_path_count)
      fputs(" and ", stdout);
    csemver_print_bold(paths[i - 1]);
  }
  if (has_changelog) {
    if (version_path_count != 0)
      fputs(" and ", stdout);
    csemver_print_bold(paths[path_count - 1]);
  }
  if (config->commit_all) {
    if (path_count > 0) {
      fputs(" and ", stdout);
      csemver_print_bold("all staged files");
    } else {
      csemver_print_bold("all staged files");
      fputs(" and %s", stdout);
    }
  }
  fputc('\n', stdout);
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
  bool publish_package;
  if (config->silent || config->skip_tag)
    return 1;
  for (i = 0; i < path_count; ++i)
    if (strcmp(paths[i], "package.json") == 0)
      updated_package = true;
  publish_package = updated_package && !is_private;
  if (!csemver_run_git(branch_args, &branch_output, &status) || status != 0 ||
      branch_output == NULL) {
    free(branch_output);
    return 1;
  }
  if (publish_package && publish_command[0] == '\0') {
    if (access("yarn.lock", F_OK) == 0)
      publish_command = "yarn publish";
    else if (access("pnpm-lock.yaml", F_OK) == 0)
      publish_command = "pnpm publish";
    else
      publish_command = "npm publish";
  }
  csemver_print_checkpoint_info();
  printf(" Run `");
  if (csemver_terminal_supports_color(stdout))
    fputs("\033[1m", stdout);
  printf("git push --follow-tags origin %s", trim(branch_output));
  free(branch_output);
  if (publish_package) {
    printf(" && %s", publish_command);
    if (config->has_prerelease)
      printf(" --tag %s", config->prerelease_id[0] == '\0'
                              ? "prerelease"
                              : config->prerelease_id);
  }
  if (csemver_terminal_supports_color(stdout))
    fputs("\033[22m", stdout);
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

static int uses_plain_text_updater(const CsemverFile *file) {
  if (file->type[0] != '\0')
    return strcmp(file->type, "plain-text") == 0;
  return strstr(file->filename, ".json") == NULL &&
         strstr(file->filename, "pyproject.toml") == NULL &&
         strstr(file->filename, ".toml") == NULL &&
         strstr(file->filename, "build.gradle") == NULL &&
         !package_path_ends_with(file->filename, ".csproj") &&
         strstr(file->filename, "pom.xml") == NULL &&
         strstr(file->filename, ".yaml") == NULL &&
         strstr(file->filename, ".yml") == NULL;
}

static int csemver_main_impl(int argc, char **argv) {
  CsemverConfig config;
  const char *config_path;
  char config_storage[CSEMVER_PATH_MAX];
  char tags[CSEMVER_COMMIT_MAX][SEMVER_TEXT_MAX];
  char latest_version[SEMVER_TEXT_MAX], latest_tag[SEMVER_TEXT_MAX];
  char lerna_tag[CSEMVER_VALUE_MAX];
  char current[SEMVER_TEXT_MAX], next[SEMVER_TEXT_MAX];
  char fallback_version[SEMVER_TEXT_MAX];
  char new_tag[SEMVER_TEXT_MAX];
  char message[CSEMVER_VALUE_MAX];
  bool is_private = false;
  bool lerna_bump = false;
  bool release_as_null = false;
  size_t tag_count = 0, commit_count = 0, path_count = 0;
  Commit *commits = NULL;
  char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX];
  Semver current_semver;
  int bump, parsed_args, package_version_status;
  FallbackTagStatus fallback_tag_status;
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
  package_version_status = get_version(&config, current, &is_private);
  if (package_version_status < 0)
    return 0;
  if (!config.skip_bump) {
    if (!release_as_is_valid(config.release_as)) {
      if (!config.silent)
        fputs("releaseAs must be one of 'major', 'minor' or 'patch', or a "
              "valid semvar version.\n",
              stderr);
      return 1;
    }
    if (!config.first_release && !validate_release_as_prerelease(&config))
      return 1;
  }
  if (!csemver_preset_is_supported(&config)) {
    char message[256];
    snprintf(message, sizeof message,
             "Unable to load the \"%s\" preset package. Please make sure "
             "it's installed.",
             config.preset);
    csemver_print_error_line(message);
    return 1;
  }
  {
    const char *args[] = {"rev-parse", "--is-inside-work-tree", NULL};
    if (!csemver_run_git(args, &check_output, &status) || status != 0 ||
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
  if (package_version_status == 0) {
    if (!config.git_tag_fallback) {
      if (!config.silent)
        fputs("no package file found\n", stderr);
      return 1;
    }
    fallback_tag_status = select_fallback_tag_version(
        &config, tags, tag_count, fallback_version, sizeof fallback_version);
    if (fallback_tag_status == FALLBACK_TAG_NULL_VERSION) {
      if (!config.silent)
        fputs("Invalid version. Must be a string. Got type \"object\".\n",
              stderr);
      return 1;
    }
    if (fallback_tag_status == FALLBACK_TAG_UNDEFINED_VERSION) {
      if (!config.silent)
        fputs("Invalid version. Must be a string. Got type \"undefined\".\n",
              stderr);
      return 1;
    }
    if (fallback_tag_status == FALLBACK_TAG_INVALID_REGEX) {
      if (!config.silent)
        fputs("Invalid tagPrefix regular expression.\n", stderr);
      return 1;
    }
    snprintf(current, sizeof current, "%s", fallback_version);
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
    if (!config.silent)
      fprintf(stderr, "Invalid Version: %s\n", current);
    return 1;
  }
  commits = calloc(CSEMVER_COMMIT_MAX, sizeof(*commits));
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
    snprintf(next, sizeof next, "%s", current);
  } else if (config.skip_bump) {
    snprintf(next, sizeof next, "%s", current);
  } else if (config.release_as[0] == '\0' && bump == 0 &&
             config.no_bump_when_empty_changes) {
    if (!config.silent) {
      csemver_print_checkpoint_cross();
      puts(" no commits found, so not bumping version");
    }
    free(commits);
    return 0;
  } else {
    if (config.release_as[0] == '\0' && bump == 0)
      bump = 1;
    if (release_as_type_case_mismatch(config.release_as)) {
      snprintf(next, sizeof next, "null");
      release_as_null = true;
    } else if (!generate_version(&config, current, bump, next, sizeof next)) {
      free(commits);
      errorf(
          "no releasable conventional commits found, or invalid release type");
      return 1;
    }
    if (!release_as_null && !resolve_unique_prerelease(&config, tags, tag_count,
                                                       next, sizeof next)) {
      free(commits);
      errorf("cannot resolve unique prerelease version");
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
  if (!config.silent && config.first_release && !config.skip_bump) {
    csemver_print_checkpoint_cross();
    puts(" skip version bump on first release");
  }
  if (!config.dry_run && !update_files(&config, next, release_as_null, paths,
                                       &path_count, false)) {
    free(commits);
    return 1;
  }
  if (release_as_null) {
    if (config.dry_run && (!update_files(&config, next, release_as_null, paths,
                                         &path_count, true) ||
                           !csemver_run_lifecycle(&config, "postbump"))) {
      free(commits);
      return 1;
    }
    free(commits);
    return 0;
  }
  if ((!config.dry_run || lerna_bump) &&
      !read_commits(&config, latest_tag[0] == '\0' ? NULL : latest_tag, commits,
                    &commit_count)) {
    free(commits);
    errorf(config.dry_run ? "cannot read Git history"
                          : "cannot reread Git history after release hooks");
    return 1;
  }
  if (config.dry_run && !config.skip_bump && !config.first_release &&
      !update_files(&config, next, false, paths, &path_count, true)) {
    free(commits);
    return 1;
  }
  if (config.dry_run && !config.skip_bump &&
      !csemver_run_lifecycle(&config, "postbump")) {
    free(commits);
    return 1;
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
        !tag_release(&config, new_tag, message, is_private, paths,
                     path_count)) {
      free(commits);
      return 1;
    }
  } else {
    if (!config.skip_commit) {
      if (!csemver_run_lifecycle(&config, "precommit")) {
        free(commits);
        return 1;
      }
      print_commit_summary(&config, paths, path_count);
      if (!csemver_run_lifecycle(&config, "postcommit")) {
        free(commits);
        return 1;
      }
    }
    if (!config.skip_tag) {
      if (!csemver_run_lifecycle(&config, "pretag")) {
        free(commits);
        return 1;
      }
      if (!config.silent) {
        csemver_print_checkpoint_tick(&config);
        printf(" tagging release ");
        csemver_print_bold(config.tag_prefix);
        csemver_print_bold(new_tag + strlen(config.tag_prefix));
        putchar('\n');
      }
      if (!print_publish_hint(&config, is_private, paths, path_count) ||
          !csemver_run_lifecycle(&config, "posttag")) {
        free(commits);
        return 1;
      }
    }
  }
  free(commits);
  (void)tag_count;
  return 0;
}

int csemver_main(int argc, char **argv) {
  int status;

  status = csemver_main_impl(argc, argv);
  return status;
}

