#ifndef CSEMVER_CHANGELOG_H
#define CSEMVER_CHANGELOG_H

#include <stdbool.h>
#include <stddef.h>

#include "common.h"
#include "config.h"
#include "semver.h"

#define CSEMVER_COMMIT_MAX 1024
#define CSEMVER_ISSUE_REFERENCE_MAX 64
#define CSEMVER_ISSUE_REFERENCE_TEXT_MAX 128

/* One parsed commit: hash, subject line, and the trimmed message body. */
typedef struct {
  char hash[64];
  char subject[2048];
  char body[4096];
} Commit;

typedef struct {
  char text[CSEMVER_ISSUE_REFERENCE_TEXT_MAX];
  bool closing;
} IssueReference;

typedef struct {
  const Commit *commit;
  const char *text;
  size_t text_length;
} BreakingNote;

typedef struct {
  int type_index;
  CsemverBuffer key;
} CommitSortKey;

/*
  Renders one release section from the commits in its range.

  Applies the configured conventional commit types, groups entries under their
  section titles, appends issue and compare links through the repository URL
  formats, and emits breaking-change notes. The rendered text is appended to
  `output`, which the caller owns.
*/
int csemver_changelog_section(const CsemverConfig *config, const Commit *commits,
                             size_t commit_count, CsemverBuffer *output);

int csemver_render_changelog(const CsemverConfig *config, const char *version,
                             const char *previous_tag, const char *new_tag,
                             const Commit *commits, size_t commit_count,
                             char tags[][SEMVER_TEXT_MAX], size_t tag_count,
                             CsemverBuffer *output);
int csemver_regenerate_all_changelogs(
    const CsemverConfig *config, const char *version, const char *previous_tag,
    const char *new_tag, const Commit *commits, size_t commit_count,
    char tags[][SEMVER_TEXT_MAX], size_t tag_count, size_t history_limit,
    const char *date, const char *base, CsemverBuffer *output);
int csemver_normalize_changelog_newlines(CsemverBuffer *output);
int csemver_run_git(const char *const args[], char **output, int *status);
int csemver_read_commits_range(const CsemverConfig *config,
                              const char *previous_tag, const char *end_ref,
                              Commit *commits, size_t *commit_count);
int csemver_append_release_heading(const CsemverConfig *config,
                                   CsemverBuffer *output, const char *base,
                                   const char *version, const char *previous_tag,
                                   const char *tag, const char *date,
                                   const char *fallback_previous);

/* Conventional-commit classification shared with the release flow. */
int csemver_type_index(const CsemverConfig *config, const char *type);
int csemver_preset_is_angular(const CsemverConfig *config);
int csemver_preset_is_supported(const CsemverConfig *config);
int csemver_body_has_breaking_note(const char *body);
int csemver_commit_is_breaking(const char *subject, const char *body);
int csemver_preset_commit_type(const CsemverConfig *config, const char *subject,
                              char *type, size_t size, char *scope,
                              size_t scope_size, const char **description);

#endif
