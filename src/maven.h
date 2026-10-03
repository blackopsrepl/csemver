#ifndef CSEMVER_MAVEN_H
#define CSEMVER_MAVEN_H

#include <stddef.h>

int csemver_maven_read_text(const char *content, char *version,
                            size_t version_size, char *error,
                            size_t error_size);
int csemver_maven_update_text(const char *content, const char *new_version,
                              char **updated, size_t *updated_size,
                              char *old_version, size_t old_version_size,
                              char *error, size_t error_size);

#endif
