#ifndef CSEMVER_SEMVER_H
#define CSEMVER_SEMVER_H

#include <stddef.h>

#define SEMVER_TEXT_MAX 256
#define SEMVER_IDENTIFIER_MAX 128

typedef struct {
  unsigned long major;
  unsigned long minor;
  unsigned long patch;
  char prerelease[SEMVER_IDENTIFIER_MAX];
  char build[SEMVER_IDENTIFIER_MAX];
  int has_prerelease;
  int has_build;
} Semver;

int semver_parse(const char *text, Semver *version);
int semver_format(const Semver *version, char *output, size_t output_size);
int semver_bump(const Semver *version, const char *release_type,
                const char *prerelease_id, char *output, size_t output_size);
int semver_compare(const char *left, const char *right);

#endif
