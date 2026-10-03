#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static int csemver_move_descriptor_above_stdio(int *descriptor) {
  int replacement;
  if (*descriptor > STDERR_FILENO)
    return 1;
  replacement = fcntl(*descriptor, F_DUPFD, STDERR_FILENO + 1);
  if (replacement < 0)
    return 0;
  close(*descriptor);
  *descriptor = replacement;
  return 1;
}

static void csemver_close_descriptor(int *descriptor) {
  if (*descriptor >= 0) {
    close(*descriptor);
    *descriptor = -1;
  }
}

int csemver_run_process_capture_streams(const char *const argv[],
                                        char **stdout_output,
                                        char **stderr_output, size_t max_buffer,
                                        int *max_buffer_stream,
                                        int *exit_code) {
  int stdout_sockets[2] = {-1, -1};
  int stderr_sockets[2] = {-1, -1};
  struct pollfd streams[2];
  CsemverBuffer stdout_buffer;
  CsemverBuffer stderr_buffer;
  CsemverBuffer *buffers[2] = {&stdout_buffer, &stderr_buffer};
  int *parent_sockets[2] = {&stdout_sockets[0], &stderr_sockets[0]};
  pid_t child = -1;
  int child_reaped = 0;
  int wait_status = 0;
  int active_streams = 0;
  int read_ok = 1;
  int success = 0;
  int max_buffer_stream_value = 0;
  size_t i;
  char chunk[4096];
  ssize_t count;
  pid_t waited;

  if (stdout_output == NULL || stderr_output == NULL)
    return 0;
  if (max_buffer_stream != NULL)
    *max_buffer_stream = 0;
  *stdout_output = NULL;
  *stderr_output = NULL;
  csemver_buffer_init(&stdout_buffer);
  csemver_buffer_init(&stderr_buffer);
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, stdout_sockets) != 0 ||
      socketpair(AF_UNIX, SOCK_STREAM, 0, stderr_sockets) != 0)
    goto cleanup;
  if (!csemver_move_descriptor_above_stdio(&stdout_sockets[0]) ||
      !csemver_move_descriptor_above_stdio(&stdout_sockets[1]) ||
      !csemver_move_descriptor_above_stdio(&stderr_sockets[0]) ||
      !csemver_move_descriptor_above_stdio(&stderr_sockets[1]))
    goto cleanup;

  child = fork();
  if (child < 0)
    goto cleanup;
  if (child == 0) {
    close(stdout_sockets[0]);
    close(stderr_sockets[0]);
    if (dup2(stdout_sockets[1], STDOUT_FILENO) < 0 ||
        dup2(stderr_sockets[1], STDERR_FILENO) < 0)
      _exit(126);
    close(stdout_sockets[1]);
    close(stderr_sockets[1]);
    execvp(argv[0], (char *const *)argv);
    perror(argv[0]);
    _exit(127);
  }

  csemver_close_descriptor(&stdout_sockets[1]);
  csemver_close_descriptor(&stderr_sockets[1]);
  streams[0].fd = stdout_sockets[0];
  streams[0].events = POLLIN;
  streams[0].revents = 0;
  streams[1].fd = stderr_sockets[0];
  streams[1].events = POLLIN;
  streams[1].revents = 0;
  active_streams = 2;
  while (active_streams > 0) {
    int ready = poll(streams, 2, -1);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      read_ok = 0;
      break;
    }
    for (i = 0; i < 2; ++i) {
      if (streams[i].fd < 0 || streams[i].revents == 0)
        continue;
      if ((streams[i].revents & POLLNVAL) != 0) {
        read_ok = 0;
        csemver_close_descriptor(parent_sockets[i]);
        streams[i].fd = -1;
        --active_streams;
        continue;
      }
      count = read(streams[i].fd, chunk, sizeof chunk);
      if (count > 0) {
        size_t append_count = (size_t)count;
        if (max_buffer > 0 &&
            (buffers[i]->length >= max_buffer ||
             append_count > max_buffer - buffers[i]->length)) {
          append_count = buffers[i]->length < max_buffer
                             ? max_buffer - buffers[i]->length
                             : 0;
          if (max_buffer_stream_value == 0) {
            max_buffer_stream_value = i == 0
                                          ? CSEMVER_CAPTURE_STDOUT_MAX_BUFFER
                                          : CSEMVER_CAPTURE_STDERR_MAX_BUFFER;
            if (max_buffer_stream != NULL)
              *max_buffer_stream = max_buffer_stream_value;
            kill(child, SIGTERM);
          }
        }
        if (append_count > 0 &&
            !csemver_buffer_append(buffers[i], chunk, append_count))
          read_ok = 0;
      } else if (count == 0) {
        csemver_close_descriptor(parent_sockets[i]);
        streams[i].fd = -1;
        --active_streams;
      } else if (errno != EINTR && errno != EAGAIN) {
        read_ok = 0;
        csemver_close_descriptor(parent_sockets[i]);
        streams[i].fd = -1;
        --active_streams;
      }
    }
  }
  if (active_streams > 0)
    goto cleanup;
  do {
    waited = waitpid(child, &wait_status, 0);
  } while (waited < 0 && errno == EINTR);
  child_reaped = 1;
  if (waited != child)
    read_ok = 0;
  if (!read_ok)
    goto cleanup;
  if (!csemver_buffer_append(&stdout_buffer, "", 0) ||
      !csemver_buffer_append(&stderr_buffer, "", 0))
    goto cleanup;
  *stdout_output = stdout_buffer.data;
  *stderr_output = stderr_buffer.data;
  stdout_buffer.data = NULL;
  stderr_buffer.data = NULL;
  if (exit_code != NULL) {
    if (WIFEXITED(wait_status))
      *exit_code = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status))
      *exit_code = 128 + WTERMSIG(wait_status);
    else
      *exit_code = 1;
  }
  success = 1;

cleanup:
  if (child > 0 && !child_reaped) {
    kill(child, SIGKILL);
    while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
    }
  }
  csemver_close_descriptor(&stdout_sockets[0]);
  csemver_close_descriptor(&stdout_sockets[1]);
  csemver_close_descriptor(&stderr_sockets[0]);
  csemver_close_descriptor(&stderr_sockets[1]);
  if (!success) {
    free(*stdout_output);
    *stdout_output = NULL;
    free(*stderr_output);
    *stderr_output = NULL;
  }
  csemver_buffer_free(&stdout_buffer);
  csemver_buffer_free(&stderr_buffer);
  return success;
}
