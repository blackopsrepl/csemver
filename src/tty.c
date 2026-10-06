#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "tty.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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

int csemver_terminal_supports_color(FILE *stream) {
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
  if (csemver_terminal_supports_color(stream))
    fprintf(stream, "\033[%sm%s\033[%sm", start, text, end);
  else
    fputs(text, stream);
}

/* Mirror upstream's printError: chalk red enabled by stdout detection even
 * though the message itself is written to stderr. */
void csemver_print_error_line(const char *text) {
  if (csemver_terminal_supports_color(stdout))
    fprintf(stderr, "\033[31m%s\033[39m\n", text);
  else
    fprintf(stderr, "%s\n", text);
}

void csemver_print_checkpoint_tick(const CsemverConfig *config) {
  print_styled(stdout, "✔", config->dry_run ? "33" : "32", "39");
}

void csemver_print_checkpoint_cross(void) {
  print_styled(stdout, "✖", "31", "39");
}

void csemver_print_checkpoint_info(void) {
  print_styled(stdout, "ℹ", "34", "39");
}

void csemver_print_bold(const char *text) {
  print_styled(stdout, text, "1", "22");
}
