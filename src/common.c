#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void csemver_buffer_init(CsemverBuffer *buffer) {
  memset(buffer, 0, sizeof(*buffer));
}

void csemver_buffer_free(CsemverBuffer *buffer) {
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

int csemver_buffer_append(CsemverBuffer *buffer, const char *text,
                          size_t length) {
  size_t required = buffer->length + length + 1;
  size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
  char *grown;

  while (capacity < required) {
    if (capacity > ((size_t)-1) / 2)
      return 0;
    capacity *= 2;
  }
  if (capacity != buffer->capacity) {
    grown = realloc(buffer->data, capacity);
    if (grown == NULL)
      return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return 1;
}

int csemver_buffer_appendf(CsemverBuffer *buffer, const char *format, ...) {
  va_list args;
  va_list copy;
  int length;
  char *text;

  va_start(args, format);
  va_copy(copy, args);
  length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (length < 0) {
    va_end(args);
    return 0;
  }
  text = malloc((size_t)length + 1);
  if (text == NULL) {
    va_end(args);
    return 0;
  }
  vsnprintf(text, (size_t)length + 1, format, args);
  va_end(args);
  if (!csemver_buffer_append(buffer, text, (size_t)length)) {
    free(text);
    return 0;
  }
  free(text);
  return 1;
}

int csemver_read_file(const char *path, char **content, size_t *length) {
  FILE *file = fopen(path, "rb");
  long file_size;
  char *data;
  size_t read_size;

  if (file == NULL)
    return 0;
  if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  data = malloc((size_t)file_size + 1);
  if (data == NULL) {
    fclose(file);
    return 0;
  }
  read_size = fread(data, 1, (size_t)file_size, file);
  if (read_size != (size_t)file_size || ferror(file)) {
    free(data);
    fclose(file);
    return 0;
  }
  fclose(file);
  data[read_size] = '\0';
  *content = data;
  if (length != NULL)
    *length = read_size;
  return 1;
}

int csemver_write_file(const char *path, const char *content, size_t length) {
  FILE *file = fopen(path, "wb");
  int ok;

  if (file == NULL)
    return 0;
  ok = fwrite(content, 1, length, file) == length && fflush(file) == 0;
  if (fclose(file) != 0)
    ok = 0;
  return ok;
}

int csemver_run_process(const char *const argv[], char **output,
                        int *exit_code) {
  int descriptors[2];
  pid_t child;
  CsemverBuffer collected;
  char chunk[4096];
  ssize_t count;
  int wait_status;
  int read_ok = 1;

  if (pipe(descriptors) != 0)
    return 0;
  child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return 0;
  }
  if (child == 0) {
    close(descriptors[0]);
    if (dup2(descriptors[1], STDOUT_FILENO) < 0)
      _exit(126);
    close(descriptors[1]);
    execvp(argv[0], (char *const *)argv);
    perror(argv[0]);
    _exit(127);
  }
  close(descriptors[1]);
  csemver_buffer_init(&collected);
  while ((count = read(descriptors[0], chunk, sizeof chunk)) != 0) {
    if (count < 0) {
      if (errno == EINTR)
        continue;
      read_ok = 0;
      break;
    }
    if (!csemver_buffer_append(&collected, chunk, (size_t)count))
      read_ok = 0;
  }
  close(descriptors[0]);
  while (waitpid(child, &wait_status, 0) < 0) {
    if (errno == EINTR)
      continue;
    csemver_buffer_free(&collected);
    return 0;
  }
  if (collected.data == NULL) {
    collected.data = calloc(1, 1);
    if (collected.data == NULL)
      return 0;
  }
  if (output != NULL)
    *output = collected.data;
  else
    csemver_buffer_free(&collected);
  if (!read_ok)
    return 0;
  if (exit_code != NULL) {
    if (WIFEXITED(wait_status))
      *exit_code = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status))
      *exit_code = 128 + WTERMSIG(wait_status);
    else
      *exit_code = 1;
  }
  return 1;
}
