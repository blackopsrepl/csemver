#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "lifecycle.h"

#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "tty.h"
#include <stdarg.h>

static void errorf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  fputs("csemver: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}

#define LIFECYCLE_SCRIPT_MAX_BUFFER (1024U * 1024U)
#define LIFECYCLE_PIPE_ERROR_FLUSH_LIMIT (64U * 1024U)

int csemver_run_lifecycle_command(const char *command, char **stdout_output,
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
  if (!csemver_terminal_supports_color(stdout)) {
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

int csemver_run_lifecycle_capture(const CsemverConfig *config, const char *name,
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
      csemver_print_checkpoint_tick(config);
      printf(" Running lifecycle script \"");
      csemver_print_bold(name);
      printf("\"\n");
      csemver_print_checkpoint_info();
      printf(" - execute command: \"");
      csemver_print_bold(config->scripts[i].command);
      printf("\"\n");
    }
    if (config->dry_run)
      continue;
    keep_output = output != NULL && *output == NULL;
    if (!csemver_run_lifecycle_command(config->scripts[i].command, &captured_output,
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

int csemver_run_lifecycle(const CsemverConfig *config, const char *name) {
  return csemver_run_lifecycle_capture(config, name, NULL);
}
