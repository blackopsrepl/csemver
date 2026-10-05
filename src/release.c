#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "release.h"

#include "common.h"
#include "config.h"
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
#define COMMIT_MAX 1024
#define ISSUE_REFERENCE_MAX 64
#define ISSUE_REFERENCE_TEXT_MAX 128
#define LIFECYCLE_SCRIPT_MAX_BUFFER (1024U * 1024U)
/* Match Node's truncated pipe flush on maxBuffer failure. */
#define LIFECYCLE_PIPE_ERROR_FLUSH_LIMIT (64U * 1024U)
#define PACKAGE_UNSUPPORTED_FILENAME "__unsupported_filename"

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
static int uses_plain_text_updater(const CsemverFile *file);
static int load_package_config_contents(CsemverConfig *config, char *contents);
static int load_json_config_contents(CsemverConfig *config, char *contents);
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

static int color_environment_flag(const char *name) {
  const char *value = getenv(name);
  const unsigned char *cursor;
  if (value == NULL)
    return -1;
  if (value[0] == '\0' || strcmp(value, "true") == 0)
    return 1;
  if (strcmp(value, "false") == 0)
    return 0;
  for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor)
    if (!isdigit(*cursor))
      return -1;
  return strtoul(value, NULL, 10) == 0 ? 0 : 1;
}

static int terminal_supports_color(FILE *stream) {
  static const char *const ci_providers[] = {
      "GITHUB_ACTIONS", "GITEA_ACTIONS", "CIRCLECI",  "TRAVIS",
      "APPVEYOR",       "GITLAB_CI",     "BUILDKITE", "DRONE"};
  static const char *const term_prefixes[] = {"screen", "xterm",  "vt100",
                                              "vt220",  "rxvt",   "color",
                                              "ansi",   "cygwin", "linux"};
  const char *term = getenv("TERM");
  const char *term_program = getenv("TERM_PROGRAM");
  int forced = color_environment_flag("FORCE_COLOR");
  size_t index;
  if (forced >= 0)
    return forced;
  if (getenv("TF_BUILD") != NULL && getenv("AGENT_NAME") != NULL)
    return 1;
  if (!isatty(fileno(stream)))
    return 0;
  if (term != NULL && strcmp(term, "dumb") == 0)
    return 0;
  if (getenv("CI") != NULL) {
    for (index = 0; index < sizeof ci_providers / sizeof ci_providers[0];
         ++index)
      if (getenv(ci_providers[index]) != NULL)
        return 1;
    return getenv("CI_NAME") != NULL &&
           strcmp(getenv("CI_NAME"), "codeship") == 0;
  }
  if (getenv("TEAMCITY_VERSION") != NULL) {
    const char *version = getenv("TEAMCITY_VERSION");
    int major = 0;
    int minor = 0;
    if (sscanf(version, "%d.%d", &major, &minor) != 2)
      return 0;
    return major >= 10 || (major == 9 && minor >= 1);
  }
  if (term != NULL &&
      (strcmp(term, "xterm-kitty") == 0 || strcmp(term, "xterm-ghostty") == 0 ||
       strcmp(term, "wezterm") == 0))
    return 1;
  if (term_program != NULL && (strcmp(term_program, "iTerm.app") == 0 ||
                               strcmp(term_program, "Apple_Terminal") == 0))
    return 1;
  if (getenv("COLORTERM") != NULL)
    return 1;
  if (term == NULL)
    return 0;
  {
    size_t length = strlen(term);
    if ((length >= 4 && strcasecmp(term + length - 4, "-256") == 0) ||
        (length >= 9 && strcasecmp(term + length - 9, "-256color") == 0))
      return 1;
  }
  for (index = 0; index < sizeof term_prefixes / sizeof term_prefixes[0];
       ++index) {
    size_t prefix_length = strlen(term_prefixes[index]);
    if (strncasecmp(term, term_prefixes[index], prefix_length) == 0)
      return 1;
  }
  return 0;
}

static void print_styled(FILE *stream, const char *text, const char *start,
                         const char *end) {
  if (terminal_supports_color(stream))
    fprintf(stream, "\033[%sm%s\033[%sm", start, text, end);
  else
    fputs(text, stream);
}

static void print_checkpoint_tick(const CsemverConfig *config) {
  print_styled(stdout, "✔", config->dry_run ? "33" : "32", "39");
}

static void print_checkpoint_cross(void) {
  print_styled(stdout, "✖", "31", "39");
}

static void print_checkpoint_info(void) {
  print_styled(stdout, "ℹ", "34", "39");
}

static void print_bold(const char *text) {
  print_styled(stdout, text, "1", "22");
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
      "Options:\n"
      "  -h, --help                    Show help                                               [boolean]\n"
      "  -v, --version                 Show version number                                     [boolean]\n"
      "      --packageFiles             [array] [default: [\"package.json\",\"bower.json\",\"manifest.json\"]]\n"
      "      --bumpFiles                                                               [array] [default:\n"
      "         [\"package.json\",\"bower.json\",\"manifest.json\",\"package-lock.json\",\"npm-shrinkwrap.json\"]]\n"
      "  -r, --release-as              Specify the release type manually (like npm version\n"
      "                                <major|minor|patch>)                                     [string]\n"
      "  -p, --prerelease              make a pre-release with optional option value to specify a tag id\n"
      "                                                                                         [string]\n"
      "  -i, --infile                  Read the CHANGELOG from this file       [default: \"CHANGELOG.md\"]\n"
      "  -m, --message                 [DEPRECATED] Commit message, replaces %s with new version.\n"
      "                                This option will be removed in the next major version, please use\n"
      "                                --releaseCommitMessageFormat.                            [string]\n"
      "  -f, --first-release           Is this the first release?             [boolean] [default: false]\n"
      "  -s, --sign                    Should the git commit and tag be signed?\n"
      "                                                                       [boolean] [default: false]\n"
      "      --signoff                 Should the git commit have a \"Signed-off-by\" trailer\n"
      "                                                                       [boolean] [default: false]\n"
      "  -n, --no-verify               Bypass pre-commit or commit-msg git hooks during the commit phase\n"
      "                                                                       [boolean] [default: false]\n"
      "  -a, --commit-all              Commit all staged changes, not just files affected by\n"
      "                                commit-and-tag-version                 [boolean] [default: false]\n"
      "      --silent                  Don't print logs and errors            [boolean] [default: false]\n"
      "  -t, --tag-prefix              Set a custom prefix for the git tag to be created\n"
      "                                                                          [string] [default: \"v\"]\n"
      "      --release-count           How many releases of changelog you want to generate. It counts\n"
      "                                from the upcoming release. Useful when you forgot to generate any\n"
      "                                previous changelog. Set to 0 to regenerate all.\n"
      "                                                                            [number] [default: 1]\n"
      "      --tag-force               Allow tag replacement                  [boolean] [default: false]\n"
      "      --scripts                 Provide scripts to execute for lifecycle events (prebump,\n"
      "                                precommit, etc.,)                                   [default: {}]\n"
      "      --skip                    Map of steps in the release process that should be skipped\n"
      "                                                                                    [default: {}]\n"
      "      --dry-run                 See the commands that running commit-and-tag-version would run\n"
      "                                                                       [boolean] [default: false]\n",
      stdout);
  fputs(
      "      --git-tag-fallback        fallback to git tags for version, if no meta-information file is\n"
      "                                found (e.g., package.json)              [boolean] [default: true]\n"
      "      --path                    Only populate commits made under this path               [string]\n"
      "      --changelogHeader         [DEPRECATED] Use a custom header when generating and updating\n"
      "                                changelog.\n"
      "                                This option will be removed in the next major version, please use\n"
      "                                --header.                                                [string]\n"
      "      --preset                  Commit message guideline preset\n"
      "                                 [string] [default: \"conventional-changelog-conventionalcommits\"]\n"
      "      --lerna-package           Name of the package from which the tags will be extracted[string]\n"
      "      --npmPublishHint          Customized publishing hint                               [string]\n"
      "  -c, --config                  Path to a custom configuration file                      [string]\n"
      "      --noBumpWhenEmptyChanges  Avoid bumping files and generating changelog if there are no\n"
      "                                changes.                               [boolean] [default: false]\n"
      "\nExamples:\n",
      stdout);
  printf("  %s                                     Update changelog and tag release\n",
         program_name);
  printf("  %s -m \"%%s: see changelog for details\"  Update changelog and tag release with custom commit\n",
         program_name);
  fputs("                                             message\n", stdout);
}
// clang-format on

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

static const char *find_default_config_path(char *storage,
                                            size_t storage_size) {
  static const char *const filenames[] = {".versionrc", ".versionrc.cjs",
                                          ".versionrc.mjs", ".versionrc.json",
                                          ".versionrc.js"};
  char directory[CSEMVER_PATH_MAX];
  int current_directory = 1;
  if (getcwd(directory, sizeof directory) == NULL)
    return NULL;
  for (;;) {
    size_t index;
    for (index = 0; index < sizeof filenames / sizeof filenames[0]; ++index) {
      char candidate[CSEMVER_PATH_MAX];
      int length =
          snprintf(candidate, sizeof candidate, "%s%s%s", directory,
                   strcmp(directory, "/") == 0 ? "" : "/", filenames[index]);
      if (length < 0 || (size_t)length >= sizeof candidate)
        continue;
      if (access(candidate, F_OK) == 0) {
        if (snprintf(storage, storage_size, "%s", candidate) >=
            (int)storage_size)
          return NULL;
        return storage;
      }
    }
    if (current_directory) {
      char candidate[CSEMVER_PATH_MAX];
      int length =
          snprintf(candidate, sizeof candidate, "%s%s%s", directory,
                   strcmp(directory, "/") == 0 ? "" : "/", "csemver.toml");
      if (length >= 0 && (size_t)length < sizeof candidate &&
          access(candidate, F_OK) == 0) {
        if (snprintf(storage, storage_size, "%s", candidate) >=
            (int)storage_size)
          return NULL;
        return storage;
      }
      current_directory = 0;
    }
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
  return NULL;
}

static int path_has_suffix(const char *path, const char *suffix) {
  size_t path_length = strlen(path);
  size_t suffix_length = strlen(suffix);
  return path_length >= suffix_length &&
         strcmp(path + path_length - suffix_length, suffix) == 0;
}

static int load_config(CsemverConfig *config, const char *path) {
  char *contents = NULL;
  char error[256] = {0};
  char discovered_path[CSEMVER_PATH_MAX];
  const char *selected_path = path;
  const char *first;
  if (selected_path == NULL) {
    selected_path =
        find_default_config_path(discovered_path, sizeof discovered_path);
    if (selected_path == NULL)
      return 1;
  }
  if (!csemver_read_file(selected_path, &contents, NULL)) {
    if (path_has_suffix(selected_path, ".toml"))
      errorf("cannot read TOML config '%s'", selected_path);
    else
      errorf("cannot read config '%s'", selected_path);
    return 0;
  }
  if (path_has_suffix(selected_path, ".js") ||
      path_has_suffix(selected_path, ".cjs") ||
      path_has_suffix(selected_path, ".mjs")) {
    errorf("JavaScript configuration files require Node and are unsupported");
    free(contents);
    return 0;
  }
  first = contents;
  while (isspace((unsigned char)*first))
    ++first;
  if (*first == '{') {
    if (!csemver_json_validate(contents)) {
      errorf("invalid JSON config '%s'", selected_path);
      free(contents);
      return 0;
    }
    return load_json_config_contents(config, contents);
  }
  if (!csemver_config_parse(config, contents, error, sizeof error)) {
    errorf("%s", error);
    free(contents);
    return 0;
  }
  free(contents);
  return 1;
}

static int package_upstream_updater_type_supported(const char *type) {
  static const char *const supported_types[] = {
      "csproj",  "gradle",     "json",   "maven",
      "openapi", "plain-text", "python", "yaml"};
  size_t index;
  for (index = 0; index < sizeof supported_types / sizeof supported_types[0];
       ++index) {
    if (strcmp(type, supported_types[index]) == 0)
      return 1;
  }
  return 0;
}

static int package_path_ends_with(const char *path, const char *suffix) {
  size_t path_length = strlen(path);
  size_t suffix_length = strlen(suffix);
  return path_length >= suffix_length &&
         strcmp(path + path_length - suffix_length, suffix) == 0;
}

static int package_path_matches_upstream_pattern(const char *path,
                                                 const char *pattern) {
  size_t path_length = strlen(path);
  size_t pattern_length = strlen(pattern);
  size_t offset;
  for (offset = 0; offset + pattern_length <= path_length; ++offset) {
    size_t index;
    for (index = 0; index < pattern_length; ++index) {
      unsigned char actual = (unsigned char)path[offset + index];
      unsigned char expected = (unsigned char)pattern[index];
      if ((expected != '.' && actual != expected) ||
          (expected == '.' && (actual == '\r' || actual == '\n')))
        break;
    }
    if (index == pattern_length)
      return 1;
  }
  return 0;
}

static const char *package_bump_file_type_from_filename(const char *filename) {
  static const char *const json_filenames[] = {
      "package.json", "bower.json", "manifest.json", "package-lock.json",
      "npm-shrinkwrap.json"};
  const char *base = strrchr(filename, '/');
  size_t index;
  base = base == NULL ? filename : base + 1;
  for (index = 0; index < sizeof json_filenames / sizeof json_filenames[0];
       ++index) {
    if (strcmp(base, json_filenames[index]) == 0)
      return "json";
  }
  if (strcmp(filename, "VERSION.txt") == 0 ||
      strcmp(filename, "version.txt") == 0)
    return "plain-text";
  if (package_path_matches_upstream_pattern(filename, "pom.xml"))
    return "maven";
  if (package_path_matches_upstream_pattern(filename, "build.gradle"))
    return "gradle";
  if (package_path_ends_with(filename, ".csproj"))
    return "csproj";
  if (package_path_matches_upstream_pattern(filename, "openapi.yaml"))
    return "openapi";
  if (package_path_ends_with(filename, ".yaml") ||
      package_path_ends_with(filename, ".yml"))
    return "yaml";
  if (package_path_matches_upstream_pattern(filename, "pyproject.toml"))
    return "python";
  return NULL;
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

static int load_package_config_contents(CsemverConfig *config, char *contents) {
  typedef struct {
    const char *json_key;
    const char *config_key;
  } PackageStringOption;
  typedef struct {
    const char *json_key;
    const char *config_key;
  } PackageBooleanOption;
  typedef struct {
    const char *json_key;
    const char *config_key;
    size_t max_values;
  } PackageArrayOption;
  static const char *const sections[] = {"commit-and-tag-version",
                                         "standard-version"};
  static const PackageStringOption options[] = {
      {"release-as", "release-as"},
      {"releaseAs", "releaseAs"},
      {"prerelease", "prerelease"},
      {"infile", "infile"},
      {"message", "message"},
      {"tag-prefix", "tag-prefix"},
      {"tagPrefix", "tagPrefix"},
      {"header", "header"},
      {"changelogHeader", "changelogHeader"},
      {"releaseCommitMessageFormat", "releaseCommitMessageFormat"},
      {"release-commit-message-format", "release-commit-message-format"},
      {"path", "path"},
      {"preset", "preset"},
      {"lerna-package", "lerna-package"},
      {"lernaPackage", "lernaPackage"},
      {"npmPublishHint", "npmPublishHint"},
      {"npm-publish-hint", "npm-publish-hint"},
      {"commitUrlFormat", "commitUrlFormat"},
      {"commit-url-format", "commit-url-format"},
      {"compareUrlFormat", "compareUrlFormat"},
      {"compare-url-format", "compare-url-format"},
      {"issueUrlFormat", "issueUrlFormat"},
      {"issue-url-format", "issue-url-format"},
      {"userUrlFormat", "userUrlFormat"},
      {"user-url-format", "user-url-format"}};
  static const PackageBooleanOption boolean_options[] = {
      {"first-release", "first-release"},
      {"firstRelease", "firstRelease"},
      {"sign", "sign"},
      {"signoff", "signoff"},
      {"no-verify", "no-verify"},
      {"noVerify", "noVerify"},
      {"commit-all", "commit-all"},
      {"commitAll", "commitAll"},
      {"silent", "silent"},
      {"tag-force", "tag-force"},
      {"tagForce", "tagForce"},
      {"dry-run", "dry-run"},
      {"dryRun", "dryRun"},
      {"git-tag-fallback", "git-tag-fallback"},
      {"gitTagFallback", "gitTagFallback"},
      {"noBumpWhenEmptyChanges", "noBumpWhenEmptyChanges"},
      {"no-bump-when-empty-changes", "no-bump-when-empty-changes"},
      {"preMajor", "preMajor"},
      {"pre-major", "pre-major"}};
  static const PackageArrayOption array_options[] = {
      {"packageFiles", "packageFiles", CSEMVER_MAX_FILES},
      {"package-files", "package-files", CSEMVER_MAX_FILES},
      {"issuePrefixes", "issuePrefixes", CSEMVER_MAX_PREFIXES},
      {"issue-prefixes", "issue-prefixes", CSEMVER_MAX_PREFIXES}};
  static const char *const numeric_options[] = {"releaseCount",
                                                "release-count"};
  static const char *const skip_options[] = {"bump", "changelog", "commit",
                                             "tag"};
  static const char *const script_options[] = {
      "prerelease", "prebump",    "postbump", "prechangelog", "postchangelog",
      "precommit",  "postcommit", "pretag",   "posttag"};
  static const char *const bump_file_options[] = {"bumpFiles", "bump-files"};
  char error[256] = {0};
  size_t section_index, option_index;
  for (section_index = 0; section_index < sizeof sections / sizeof sections[0];
       ++section_index) {
    for (option_index = 0; option_index < sizeof options / sizeof options[0];
         ++option_index) {
      char value[CSEMVER_VALUE_MAX];
      if (!csemver_json_object_string(contents, sections[section_index],
                                      options[option_index].json_key, value,
                                      sizeof value))
        continue;
      if (!csemver_config_set_string(config, options[option_index].config_key,
                                     value, error, sizeof error)) {
        errorf("%s", error);
        free(contents);
        return 0;
      }
    }
    for (option_index = 0;
         option_index < sizeof boolean_options / sizeof boolean_options[0];
         ++option_index) {
      bool value;
      if (!csemver_json_object_boolean(contents, sections[section_index],
                                       boolean_options[option_index].json_key,
                                       &value))
        continue;
      if (!csemver_config_set_bool(config,
                                   boolean_options[option_index].config_key,
                                   value, error, sizeof error)) {
        errorf("%s", error);
        free(contents);
        return 0;
      }
    }
    for (option_index = 0;
         option_index < sizeof skip_options / sizeof skip_options[0];
         ++option_index) {
      bool value;
      char key[64];
      if (!csemver_json_object_nested_boolean(
              contents, sections[section_index], "skip",
              skip_options[option_index], &value))
        continue;
      snprintf(key, sizeof key, "skip.%s", skip_options[option_index]);
      if (!csemver_config_set_bool(config, key, value, error, sizeof error)) {
        errorf("%s", error);
        free(contents);
        return 0;
      }
    }
    for (option_index = 0;
         option_index < sizeof script_options / sizeof script_options[0];
         ++option_index) {
      char command[CSEMVER_VALUE_MAX];
      if (!csemver_json_object_nested_string(
              contents, sections[section_index], "scripts",
              script_options[option_index], command, sizeof command))
        continue;
      if (!csemver_config_set_script(config, script_options[option_index],
                                     command, error, sizeof error)) {
        errorf("%s", error);
        free(contents);
        return 0;
      }
    }
    for (option_index = 0;
         option_index < sizeof array_options / sizeof array_options[0];
         ++option_index) {
      char values[CSEMVER_MAX_FILES][CSEMVER_PATH_MAX];
      char file_types[CSEMVER_MAX_FILES][32] = {{0}};
      bool package_file_objects[CSEMVER_MAX_FILES] = {false};
      bool package_file_type_precedes_filename[CSEMVER_MAX_FILES] = {false};
      char *package_file_argument_json[CSEMVER_MAX_FILES] = {NULL};
      bool package_file_argument_json_valid[CSEMVER_MAX_FILES] = {false};
      bool package_files_have_custom_updater = false;
      bool unsupported_types[CSEMVER_MAX_FILES] = {false};
      bool unsupported_filenames[CSEMVER_MAX_FILES] = {false};
      const char *value_pointers[CSEMVER_MAX_FILES];
      size_t value_count, value_index;
      int typed_package_files = 0;
      int package_files_option =
          strcmp(array_options[option_index].config_key, "packageFiles") == 0 ||
          strcmp(array_options[option_index].config_key, "package-files") == 0;
      if (!csemver_json_object_string_array(
              contents, sections[section_index],
              array_options[option_index].json_key, &values[0][0],
              sizeof values[0], array_options[option_index].max_values,
              &value_count)) {
        if ((strcmp(array_options[option_index].config_key, "packageFiles") !=
             0) &&
            (strcmp(array_options[option_index].config_key, "package-files") !=
             0))
          continue;
        if (!csemver_json_object_mixed_file_array(
                contents, sections[section_index],
                array_options[option_index].json_key, &values[0][0],
                sizeof values[0], &file_types[0][0], sizeof file_types[0],
                package_file_objects, package_file_type_precedes_filename,
                package_file_argument_json, package_file_argument_json_valid,
                &package_files_have_custom_updater,
                array_options[option_index].max_values, &value_count))
          continue;
        if (package_files_have_custom_updater) {
          errorf("custom JavaScript updaters require Node and are unsupported");
          free(contents);
          return 0;
        }
        typed_package_files = 1;
      }
      for (value_index = 0; value_index < value_count; ++value_index) {
        if (typed_package_files && file_types[value_index][0] != '\0' &&
            !package_upstream_updater_type_supported(file_types[value_index]))
          unsupported_types[value_index] = true;
        if (package_files_option && file_types[value_index][0] == '\0') {
          const char *inferred_type =
              package_bump_file_type_from_filename(values[value_index]);
          if (inferred_type == NULL)
            unsupported_filenames[value_index] = true;
          else
            snprintf(file_types[value_index], sizeof file_types[value_index],
                     "%s", inferred_type);
        }
        value_pointers[value_index] = values[value_index];
      }
      if (!csemver_config_set_array(
              config, array_options[option_index].config_key, value_pointers,
              value_count, error, sizeof error)) {
        errorf("%s", error);
        free(contents);
        return 0;
      }
      if (package_files_option) {
        size_t file_index;
        for (value_index = 0; value_index < value_count; ++value_index) {
          for (file_index = 0; file_index < config->package_file_count;
               ++file_index) {
            if (strcmp(config->package_files[file_index].filename,
                       values[value_index]) == 0) {
              if (file_types[value_index][0] != '\0')
                memcpy(config->package_files[file_index].type,
                       file_types[value_index], sizeof file_types[value_index]);
              config->package_files[file_index].compatibility_unsupported_type =
                  unsupported_types[value_index];
              config->package_files[file_index]
                  .compatibility_unsupported_filename =
                  unsupported_filenames[value_index];
              config->package_files[file_index]
                  .compatibility_updater_argument_object =
                  package_file_objects[value_index];
              config->package_files[file_index]
                  .compatibility_updater_type_precedes_filename =
                  package_file_type_precedes_filename[value_index];
              config->package_files[file_index]
                  .compatibility_updater_argument_json_valid =
                  package_file_argument_json_valid[value_index];
              if (package_file_argument_json_valid[value_index])
                config->package_files[file_index]
                    .compatibility_updater_argument_json =
                    package_file_argument_json[value_index];
            }
          }
        }
      }
    }
    for (option_index = 0;
         option_index < sizeof bump_file_options / sizeof bump_file_options[0];
         ++option_index) {
      char filenames[CSEMVER_MAX_FILES][CSEMVER_PATH_MAX];
      char types[CSEMVER_MAX_FILES][32];
      bool type_precedes_filename[CSEMVER_MAX_FILES] = {false};
      char *argument_json[CSEMVER_MAX_FILES] = {NULL};
      bool argument_json_valid[CSEMVER_MAX_FILES] = {false};
      bool has_custom_updater = false;
      bool unsupported_types[CSEMVER_MAX_FILES] = {false};
      const char *value_pointers[CSEMVER_MAX_FILES];
      size_t value_count, value_index;
      int typed_files = csemver_json_object_mixed_file_array(
          contents, sections[section_index], bump_file_options[option_index],
          &filenames[0][0], sizeof filenames[0], &types[0][0], sizeof types[0],
          NULL, type_precedes_filename, argument_json, argument_json_valid,
          &has_custom_updater, CSEMVER_MAX_FILES, &value_count);
      if (typed_files && has_custom_updater) {
        errorf("custom JavaScript updaters require Node and are unsupported");
        free(contents);
        return 0;
      }
      if (!typed_files) {
        if (!csemver_json_object_string_array(
                contents, sections[section_index],
                bump_file_options[option_index], &filenames[0][0],
                sizeof filenames[0], CSEMVER_MAX_FILES, &value_count))
          continue;
        for (value_index = 0; value_index < value_count; ++value_index) {
          const char *type =
              package_bump_file_type_from_filename(filenames[value_index]);
          if (type == NULL)
            type = PACKAGE_UNSUPPORTED_FILENAME;
          snprintf(types[value_index], sizeof types[value_index], "%s", type);
        }
      }
      for (value_index = 0; value_index < value_count; ++value_index) {
        if (types[value_index][0] == '\0') {
          const char *type =
              package_bump_file_type_from_filename(filenames[value_index]);
          if (type == NULL)
            type = PACKAGE_UNSUPPORTED_FILENAME;
          snprintf(types[value_index], sizeof types[value_index], "%s", type);
        }
        unsupported_types[value_index] =
            strcmp(types[value_index], PACKAGE_UNSUPPORTED_FILENAME) != 0 &&
            !package_upstream_updater_type_supported(types[value_index]);
        value_pointers[value_index] = filenames[value_index];
      }
      if (!csemver_config_set_array(config, "bumpFiles", value_pointers,
                                    value_count, error, sizeof error)) {
        errorf("%s", error);
        free(contents);
        return 0;
      }
      for (value_index = 0; value_index < value_count; ++value_index)
        snprintf(config->bump_files[value_index].type,
                 sizeof config->bump_files[value_index].type, "%s",
                 types[value_index]);
      for (value_index = 0; value_index < value_count; ++value_index)
        config->bump_files[value_index].compatibility_unsupported_type =
            unsupported_types[value_index];
      for (value_index = 0; value_index < value_count; ++value_index)
        config->bump_files[value_index]
            .compatibility_updater_type_precedes_filename =
            type_precedes_filename[value_index];
      for (value_index = 0; value_index < value_count; ++value_index) {
        config->bump_files[value_index]
            .compatibility_updater_argument_json_valid =
            argument_json_valid[value_index];
        if (argument_json_valid[value_index])
          config->bump_files[value_index].compatibility_updater_argument_json =
              argument_json[value_index];
      }
    }
    for (option_index = 0;
         option_index < sizeof numeric_options / sizeof numeric_options[0];
         ++option_index) {
      unsigned value;
      if (csemver_json_object_unsigned(contents, sections[section_index],
                                       numeric_options[option_index], &value))
        config->release_count = value;
    }
    {
      char types[CSEMVER_MAX_TYPES][64];
      char type_sections[CSEMVER_MAX_TYPES][128];
      bool hidden[CSEMVER_MAX_TYPES];
      bool bump[CSEMVER_MAX_TYPES];
      size_t type_count, type_index;
      if (csemver_json_object_commit_type_array(
              contents, sections[section_index], "types", &types[0][0],
              sizeof types[0], &type_sections[0][0], sizeof type_sections[0],
              hidden, bump, CSEMVER_MAX_TYPES, &type_count)) {
        memset(config->commit_types, 0, sizeof config->commit_types);
        config->commit_type_count = type_count;
        for (type_index = 0; type_index < type_count; ++type_index) {
          snprintf(config->commit_types[type_index].type,
                   sizeof config->commit_types[type_index].type, "%s",
                   types[type_index]);
          snprintf(config->commit_types[type_index].section,
                   sizeof config->commit_types[type_index].section, "%s",
                   type_sections[type_index]);
          config->commit_types[type_index].hidden = hidden[type_index];
          config->commit_types[type_index].bump = bump[type_index];
        }
      }
    }
  }
  free(contents);
  return 1;
}

static int load_package_config(CsemverConfig *config) {
  char *contents = NULL;
  if (!csemver_read_file("package.json", &contents, NULL))
    return 1;
  return load_package_config_contents(config, contents);
}

static int load_json_config_contents(CsemverConfig *config, char *contents) {
  static const char prefix[] = "{\"commit-and-tag-version\":";
  size_t prefix_length = sizeof prefix - 1;
  size_t content_length = strlen(contents);
  char *wrapped;
  if (content_length > SIZE_MAX - prefix_length - 2) {
    errorf("configuration file is too large");
    free(contents);
    return 0;
  }
  wrapped = malloc(prefix_length + content_length + 2);
  if (wrapped == NULL) {
    errorf("out of memory");
    free(contents);
    return 0;
  }
  memcpy(wrapped, prefix, prefix_length);
  memcpy(wrapped + prefix_length, contents, content_length);
  wrapped[prefix_length + content_length] = '}';
  wrapped[prefix_length + content_length + 1] = '\0';
  free(contents);
  return load_package_config_contents(config, wrapped);
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
  if (!run_git(args, &output, &status) || status != 0) {
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
      if (*tag_count < COMMIT_MAX)
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

static int run_lifecycle_command(const char *command, char **stdout_output,
                                 char **stderr_output, int *max_buffer_stream,
                                 int *exit_code) {
  const char *argv[] = {"/bin/sh", "-c", command, NULL};
  return csemver_run_process_capture_streams(argv, stdout_output, stderr_output,
                                             LIFECYCLE_SCRIPT_MAX_BUFFER,
                                             max_buffer_stream, exit_code);
}

static void print_lifecycle_message(const char *message, const char *color) {
  const char *cursor;
  if (message[0] == '\0') {
    fputc('\n', stderr);
    return;
  }
  if (!terminal_supports_color(stdout)) {
    fprintf(stderr, "%s\n", message);
    return;
  }
  fprintf(stderr, "\033[%sm", color);
  cursor = message;
  while (*cursor != '\0') {
    if (strncmp(cursor, "\033[39m", 5) == 0) {
      fprintf(stderr, "\033[%sm", color);
      cursor += 5;
    } else if (cursor[0] == '\r' && cursor[1] == '\n') {
      fputs("\033[39m\r\n", stderr);
      fprintf(stderr, "\033[%sm", color);
      cursor += 2;
    } else if (*cursor == '\n') {
      fputs("\033[39m\n", stderr);
      fprintf(stderr, "\033[%sm", color);
      ++cursor;
    } else {
      fputc(*cursor, stderr);
      ++cursor;
    }
  }
  fputs("\033[39m\n", stderr);
}

static int lifecycle_stderr_is_pipe(void) {
  struct stat status;
  if (fstat(STDERR_FILENO, &status) != 0)
    return 0;
  return S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode);
}

static void print_lifecycle_max_buffer_error(const CsemverConfig *config,
                                             int max_buffer_stream,
                                             const char *captured_error,
                                             const char *error_message) {
  if (config->silent)
    return;
  if (max_buffer_stream == CSEMVER_CAPTURE_STDERR_MAX_BUFFER &&
      lifecycle_stderr_is_pipe()) {
    size_t length = strlen(captured_error);
    if (length > LIFECYCLE_PIPE_ERROR_FLUSH_LIMIT)
      length = LIFECYCLE_PIPE_ERROR_FLUSH_LIMIT;
    (void)fwrite(captured_error, 1, length, stderr);
    (void)fflush(stderr);
    return;
  }
  print_lifecycle_message(
      captured_error[0] != '\0' ? captured_error : error_message, "31");
  print_lifecycle_message(error_message, "31");
}

static char *lifecycle_error_message(const char *command,
                                     const char *stderr_text) {
  CsemverBuffer message;
  csemver_buffer_init(&message);
  if (!csemver_buffer_append(&message, "Command failed: ", 16) ||
      !csemver_buffer_append(&message, command, strlen(command)) ||
      !csemver_buffer_append(&message, "\n", 1))
    goto failure;
  if (stderr_text[0] != '\0' &&
      !csemver_buffer_append(&message, stderr_text, strlen(stderr_text)))
    goto failure;
  return message.data;

failure:
  csemver_buffer_free(&message);
  return NULL;
}

static int run_lifecycle_capture(const CsemverConfig *config, const char *name,
                                 char **output) {
  size_t i;
  if (output != NULL)
    *output = NULL;
  for (i = 0; i < config->script_count; ++i) {
    int status = 0;
    int max_buffer_stream = 0;
    char *captured_output = NULL;
    char *captured_error = NULL;
    char *error_message;
    int keep_output;
    if (strcmp(config->scripts[i].name, name) != 0)
      continue;
    if (!config->silent) {
      print_checkpoint_tick(config);
      printf(" Running lifecycle script \"");
      print_bold(name);
      printf("\"\n");
      print_checkpoint_info();
      printf(" - execute command: \"");
      print_bold(config->scripts[i].command);
      printf("\"\n");
    }
    if (config->dry_run)
      continue;
    keep_output = output != NULL && *output == NULL;
    if (!run_lifecycle_command(config->scripts[i].command, &captured_output,
                               &captured_error, &max_buffer_stream, &status)) {
      errorf("unable to capture lifecycle script output");
      free(captured_output);
      free(captured_error);
      return 0;
    }
    if (max_buffer_stream != 0) {
      const char *max_buffer_error =
          max_buffer_stream == CSEMVER_CAPTURE_STDOUT_MAX_BUFFER
              ? "stdout maxBuffer length exceeded"
              : "stderr maxBuffer length exceeded";
      print_lifecycle_max_buffer_error(config, max_buffer_stream,
                                       captured_error, max_buffer_error);
      free(captured_output);
      free(captured_error);
      return 0;
    }
    if (status != 0) {
      error_message =
          lifecycle_error_message(config->scripts[i].command, captured_error);
      if (!config->silent && error_message != NULL) {
        print_lifecycle_message(
            captured_error[0] != '\0' ? captured_error : error_message, "31");
        print_lifecycle_message(error_message, "31");
      }
      free(error_message);
      free(captured_output);
      free(captured_error);
      return 0;
    }
    if (!config->silent && captured_error[0] != '\0')
      print_lifecycle_message(captured_error, "33");
    if (keep_output)
      *output = captured_output;
    else
      free(captured_output);
    free(captured_error);
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
  if (config->skip_bump)
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
    *handled = 1;
    prefix_length = (size_t)(group_start - pattern);
    suffix = close + 1;
    suffix_length = strlen(suffix);
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

static int file_is_gitignored(const char *filename) {
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
    return dry_run ? 1 : run_lifecycle(config, "postbump");
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
    if (file_is_gitignored(config->bump_files[i].filename)) {
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
      print_checkpoint_tick(config);
      printf(" bumping version in ");
      printf("%s", config->bump_files[i].filename);
      printf(" from ");
      print_bold(display_old);
      printf(" to ");
      print_bold(version);
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
  return dry_run ? 1 : run_lifecycle(config, "postbump");
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
  if (!run_lifecycle(config, "prechangelog"))
    return 0;
  if (!config->silent && access(config->infile, F_OK) != 0) {
    print_checkpoint_tick(config);
    printf(" created ");
    print_bold(config->infile);
    putchar('\n');
  }
  csemver_buffer_init(&content);
  if (!render_changelog(config, version, previous_tag, new_tag, commits,
                        commit_count, tags, tag_count, &content)) {
    csemver_buffer_free(&content);
    errorf("failed to generate changelog");
    return 0;
  }
  if (!config->silent) {
    print_checkpoint_tick(config);
    printf(" outputting changes to ");
    print_bold(config->infile);
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
    ok = run_lifecycle(config, "postchangelog");
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
  if (!run_lifecycle_capture(config, "precommit", &hook_message)) {
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
  return run_lifecycle(config, "postcommit");
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
  if (!run_lifecycle(config, "pretag"))
    return 0;
  if (!config->silent) {
    print_checkpoint_tick(config);
    printf(" tagging release ");
    print_bold(config->tag_prefix);
    print_bold(tag + strlen(config->tag_prefix));
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
  return run_lifecycle(config, "posttag");
}

static void
print_commit_summary(const CsemverConfig *config,
                     char paths[CSEMVER_MAX_FILES + 1][CSEMVER_PATH_MAX],
                     size_t path_count) {
  if (config->silent || config->skip_commit)
    return;
  print_checkpoint_tick(config);
  fputs(" committing ", stdout);
  bool has_changelog = !config->skip_changelog && path_count > 0 &&
                       strcmp(paths[path_count - 1], config->infile) == 0;
  size_t version_path_count = path_count - (has_changelog ? 1 : 0);
  for (size_t i = version_path_count; i > 0; --i) {
    if (i != version_path_count)
      fputs(" and ", stdout);
    print_bold(paths[i - 1]);
  }
  if (has_changelog) {
    if (version_path_count != 0)
      fputs(" and ", stdout);
    print_bold(paths[path_count - 1]);
  }
  if (config->commit_all) {
    if (path_count > 0) {
      fputs(" and ", stdout);
      print_bold("all staged files");
    } else {
      print_bold("all staged files");
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
  if (!run_git(branch_args, &branch_output, &status) || status != 0 ||
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
  print_checkpoint_info();
  printf(" Run `");
  if (terminal_supports_color(stdout))
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
  if (terminal_supports_color(stdout))
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
         strstr(file->filename, ".yaml") == NULL &&
         strstr(file->filename, ".yml") == NULL;
}

static int csemver_main_impl(int argc, char **argv) {
  CsemverConfig config;
  const char *config_path;
  char config_storage[CSEMVER_PATH_MAX];
  char tags[COMMIT_MAX][SEMVER_TEXT_MAX];
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
  if (!load_package_config(&config) || !load_config(&config, config_path))
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
    if (!config.silent) {
      print_checkpoint_cross();
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
    print_checkpoint_cross();
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
                           !run_lifecycle(&config, "postbump"))) {
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
      !run_lifecycle(&config, "postbump")) {
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
      if (!run_lifecycle(&config, "precommit")) {
        free(commits);
        return 1;
      }
      print_commit_summary(&config, paths, path_count);
      if (!run_lifecycle(&config, "postcommit")) {
        free(commits);
        return 1;
      }
    }
    if (!config.skip_tag) {
      if (!run_lifecycle(&config, "pretag")) {
        free(commits);
        return 1;
      }
      if (!config.silent) {
        print_checkpoint_tick(&config);
        printf(" tagging release ");
        print_bold(config.tag_prefix);
        print_bold(new_tag + strlen(config.tag_prefix));
        putchar('\n');
      }
      if (!print_publish_hint(&config, is_private, paths, path_count) ||
          !run_lifecycle(&config, "posttag")) {
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

  csemver_json_diagnostics_clear();
  status = csemver_main_impl(argc, argv);
  csemver_json_diagnostics_clear();
  return status;
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
                                  const char *tag, const char *date) {
  if (previous_tag != NULL)
    return append_compare_heading(config, output, base, version, previous_tag,
                                  tag, date);
  if (preset_is_angular(config))
    return csemver_buffer_appendf(output, "%s %s (%s)\n\n",
                                  release_heading_level(config, version),
                                  version, date);
  return csemver_buffer_appendf(output, "## %s (%s)\n\n", version, date);
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
                                new_tag, date) ||
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
                                older_tag, tags[i], current_date) ||
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
    if (!config->dry_run && output->length > 0 &&
        !csemver_buffer_append(output, "\n", 1))
      goto fail;
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
  if (previous_tag != NULL && strcmp(previous_tag, new_tag) == 0) {
    if (!config->dry_run &&
        (!csemver_buffer_append(output, "\n", 1) ||
         (*old_body != '\0' &&
          !csemver_buffer_append(output, old_body,
                                 old_length -
                                     (size_t)(old_body - old_content)))))
      goto fail;
    if ((*old_body != '\0' || config->dry_run) &&
        !normalize_changelog_newlines(output))
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
                                date))
      goto fail;
  } else if (!append_release_heading(config, output, base, version, NULL,
                                     new_tag, date))
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
