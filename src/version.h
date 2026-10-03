#ifndef CSEMVER_VERSION_H
#define CSEMVER_VERSION_H

#include <stdbool.h>
#include <stddef.h>

int csemver_version_read_text(const char *filename, const char *type,
                              const char *content, char *version,
                              size_t version_size, bool *is_private,
                              char *error, size_t error_size);
int csemver_version_update_text(const char *filename, const char *type,
                                const char *content, const char *new_version,
                                char **updated, size_t *updated_size,
                                char *old_version, size_t old_version_size,
                                char *error, size_t error_size);
int csemver_json_repository_url(const char *content, char *url,
                                size_t url_size);
int csemver_json_object_string(const char *content, const char *object_key,
                               const char *field_key, char *value,
                               size_t value_size);
int csemver_json_object_boolean(const char *content, const char *object_key,
                                const char *field_key, bool *value);
int csemver_json_object_string_array(const char *content,
                                     const char *object_key,
                                     const char *field_key, char *values,
                                     size_t value_stride, size_t max_values,
                                     size_t *value_count);
int csemver_json_object_unsigned(const char *content, const char *object_key,
                                 const char *field_key, unsigned *value);
int csemver_json_object_commit_type_array(
    const char *content, const char *object_key, const char *field_key,
    char *types, size_t type_stride, char *sections, size_t section_stride,
    bool *hidden, bool *bump, size_t max_types, size_t *type_count);
int csemver_json_object_typed_file_array(const char *content,
                                         const char *object_key,
                                         const char *field_key, char *filenames,
                                         size_t filename_stride, char *types,
                                         size_t type_stride, size_t max_values,
                                         size_t *file_count);

#endif
