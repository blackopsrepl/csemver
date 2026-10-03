#ifndef CSEMVER_COMMON_H
#define CSEMVER_COMMON_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} CsemverBuffer;

void csemver_buffer_init(CsemverBuffer *buffer);
void csemver_buffer_free(CsemverBuffer *buffer);
int csemver_buffer_append(CsemverBuffer *buffer, const char *text, size_t length);
int csemver_buffer_appendf(CsemverBuffer *buffer, const char *format, ...);
int csemver_read_file(const char *path, char **content, size_t *length);
int csemver_write_file(const char *path, const char *content, size_t length);
int csemver_run_process(const char *const argv[], char **output, int *exit_code);
int csemver_run_process_capture_streams(const char *const argv[],
                                        char **stdout_output,
                                        char **stderr_output, int *exit_code);

#endif
