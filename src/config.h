#ifndef CSEMVER_CONFIG_H
#define CSEMVER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define CSEMVER_MAX_FILES 64
#define CSEMVER_MAX_TYPES 32
#define CSEMVER_MAX_PREFIXES 16
#define CSEMVER_MAX_SCRIPT 10
#define CSEMVER_PATH_MAX 1024
#define CSEMVER_VALUE_MAX 2048

typedef struct {
  char filename[CSEMVER_PATH_MAX];
  char type[32];
} CsemverFile;

typedef struct {
  char type[64];
  char section[128];
  bool hidden;
  bool bump;
} CsemverCommitType;

typedef struct {
  char name[32];
  char command[CSEMVER_VALUE_MAX];
} CsemverScript;

typedef struct {
  CsemverFile package_files[CSEMVER_MAX_FILES];
  size_t package_file_count;
  CsemverFile bump_files[CSEMVER_MAX_FILES];
  size_t bump_file_count;
  CsemverCommitType commit_types[CSEMVER_MAX_TYPES];
  size_t commit_type_count;
  char issue_prefixes[CSEMVER_MAX_PREFIXES][64];
  size_t issue_prefix_count;
  CsemverScript scripts[CSEMVER_MAX_SCRIPT];
  size_t script_count;
  bool skip_bump;
  bool skip_changelog;
  bool skip_commit;
  bool skip_tag;
  bool first_release;
  bool sign;
  bool signoff;
  bool no_verify;
  bool commit_all;
  bool silent;
  bool tag_force;
  bool dry_run;
  bool git_tag_fallback;
  bool no_bump_when_empty_changes;
  bool pre_major;
  bool has_prerelease;
  char prerelease_id[128];
  char tag_prefix[256];
  char infile[CSEMVER_PATH_MAX];
  char header[CSEMVER_VALUE_MAX];
  char release_as[128];
  char release_commit_message_format[512];
  char message[512];
  char path[CSEMVER_PATH_MAX];
  char preset[128];
  char lerna_package[256];
  char npm_publish_hint[512];
  char commit_url_format[512];
  char compare_url_format[512];
  char issue_url_format[512];
  char user_url_format[512];
  unsigned release_count;
  bool has_message;
  bool has_changelog_header;
  bool commit_url_format_explicit;
  bool compare_url_format_explicit;
  bool issue_url_format_explicit;
  bool user_url_format_explicit;
  bool package_files_explicit;
  bool bump_files_explicit;
} CsemverConfig;

void csemver_config_defaults(CsemverConfig *config);
int csemver_config_parse(CsemverConfig *config, const char *toml, char *error,
                         size_t error_size);
int csemver_config_set_string(CsemverConfig *config, const char *key,
                              const char *value, char *error,
                              size_t error_size);
int csemver_config_set_script(CsemverConfig *config, const char *name,
                              const char *command, char *error,
                              size_t error_size);
int csemver_config_set_bool(CsemverConfig *config, const char *key, bool value,
                            char *error, size_t error_size);
int csemver_config_set_array(CsemverConfig *config, const char *key,
                             const char *const *values, size_t count,
                             char *error, size_t error_size);

#endif
