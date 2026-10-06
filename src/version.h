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

#endif
