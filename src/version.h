#ifndef CSEMVER_VERSION_H
#define CSEMVER_VERSION_H

#include <stdbool.h>
#include <stddef.h>

int csemver_version_read_text(const char *filename, const char *type,
                              const char *content, char *version,
                              size_t version_size, bool *is_private,
                              char *error, size_t error_size);
/* A NULL new_version writes a JSON null value or literal "null" text. */
int csemver_version_update_text(const char *filename, const char *type,
                                const char *content, const char *new_version,
                                char **updated, size_t *updated_size,
                                char *old_version, size_t old_version_size,
                                char *error, size_t error_size);
int csemver_version_read_pattern_text(const char *content,
                                      const char *pattern,
                                      unsigned version_group, char *version,
                                      size_t version_size, char *error,
                                      size_t error_size);
int csemver_version_update_pattern_text(
    const char *content, const char *pattern, unsigned version_group,
    const char *new_version, char **updated, size_t *updated_size,
    char *old_version, size_t old_version_size, char *error,
    size_t error_size);
int csemver_json_repository_url(const char *content, char *url,
                                size_t url_size);
int csemver_json_validate(const char *content);
int csemver_json_object_nested_string(const char *content,
                                      const char *object_key,
                                      const char *nested_key,
                                      const char *field_key, char *value,
                                      size_t value_size);
int csemver_json_object_string(const char *content, const char *object_key,
                               const char *field_key, char *value,
                               size_t value_size);
int csemver_json_object_nested_boolean(const char *content,
                                       const char *object_key,
                                       const char *nested_key,
                                       const char *field_key, bool *value);
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
int csemver_json_object_mixed_file_array(
    const char *content, const char *object_key, const char *field_key,
    char *filenames, size_t filename_stride, char *types, size_t type_stride,
    bool *is_object, bool *type_precedes_filename, char **argument_json,
    bool *argument_json_valid, bool *has_custom_updater, size_t max_values,
    size_t *file_count);
/* Frees dynamic warning arguments returned by JSON package-config parsing. */
void csemver_json_diagnostics_clear(void);

#endif
