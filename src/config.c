#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include "toml.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t size, const char *message) {
  if (error != NULL && size > 0)
    snprintf(error, size, "%s", message);
}

static int copy_text(char *target, size_t size, const char *value, char *error,
                     size_t error_size) {
  size_t length;

  if (value == NULL || size == 0)
    return 0;
  length = strlen(value);
  if (length >= size) {
    set_error(error, error_size, "configuration value is too long");
    return 0;
  }
  memcpy(target, value, length + 1);
  return 1;
}

static const char *const lifecycle_script_names[] = {
    "prerelease", "prebump",    "postbump", "prechangelog", "postchangelog",
    "precommit",  "postcommit", "pretag",   "posttag"};

static int add_file(CsemverFile *files, size_t *count, const char *filename,
                    const char *type, char *error, size_t error_size) {
  CsemverFile *file;

  if (*count >= CSEMVER_MAX_FILES) {
    set_error(error, error_size, "too many version files");
    return 0;
  }
  file = &files[*count];
  memset(file, 0, sizeof *file);
  if (!copy_text(file->filename, sizeof file->filename, filename, error,
                 error_size))
    return 0;
  if (type != NULL &&
      !copy_text(file->type, sizeof file->type, type, error, error_size))
    return 0;
  ++*count;
  return 1;
}

static void add_default_type(CsemverConfig *config, const char *type,
                             const char *section, bool hidden) {
  CsemverCommitType *entry = &config->commit_types[config->commit_type_count++];
  snprintf(entry->type, sizeof entry->type, "%s", type);
  snprintf(entry->section, sizeof entry->section, "%s", section);
  entry->hidden = hidden;
  entry->bump = !hidden;
}

void csemver_config_defaults(CsemverConfig *config) {
  memset(config, 0, sizeof(*config));
  snprintf(config->tag_prefix, sizeof config->tag_prefix, "v");
  snprintf(config->infile, sizeof config->infile, "CHANGELOG.md");
  snprintf(config->release_commit_message_format,
           sizeof config->release_commit_message_format,
           "chore(release): {{currentTag}}");
  snprintf(config->header, sizeof config->header,
           "# Changelog\n\nAll notable changes to this project will be "
           "documented in this file. See "
           "[commit-and-tag-version](https://github.com/absolute-version/"
           "commit-and-tag-version) for commit guidelines.\n");
  snprintf(config->preset, sizeof config->preset,
           "conventional-changelog-conventionalcommits");
  snprintf(config->commit_url_format, sizeof config->commit_url_format,
           "{{host}}/{{owner}}/{{repository}}/commit/{{hash}}");
  snprintf(config->compare_url_format, sizeof config->compare_url_format,
           "{{host}}/{{owner}}/{{repository}}/compare/"
           "{{previousTag}}...{{currentTag}}");
  snprintf(config->issue_url_format, sizeof config->issue_url_format,
           "{{host}}/{{owner}}/{{repository}}/issues/{{id}}");
  snprintf(config->user_url_format, sizeof config->user_url_format,
           "{{host}}/{{user}}");
  config->release_count = 1;
  config->git_tag_fallback = true;
  config->issue_prefix_count = 1;
  snprintf(config->issue_prefixes[0], sizeof config->issue_prefixes[0], "#");
  add_default_type(config, "feat", "Features", false);
  add_default_type(config, "fix", "Bug Fixes", false);
  add_default_type(config, "chore", "", true);
  add_default_type(config, "docs", "", true);
  add_default_type(config, "style", "", true);
  add_default_type(config, "refactor", "", true);
  add_default_type(config, "perf", "", true);
  add_default_type(config, "test", "", true);
  add_file(config->package_files, &config->package_file_count, "package.json",
           "json", NULL, 0);
  add_file(config->package_files, &config->package_file_count, "bower.json",
           "json", NULL, 0);
  add_file(config->package_files, &config->package_file_count, "manifest.json",
           "json", NULL, 0);
  add_file(config->bump_files, &config->bump_file_count, "package.json", "json",
           NULL, 0);
  add_file(config->bump_files, &config->bump_file_count, "bower.json", "json",
           NULL, 0);
  add_file(config->bump_files, &config->bump_file_count, "manifest.json",
           "json", NULL, 0);
  add_file(config->bump_files, &config->bump_file_count, "package-lock.json",
           "json", NULL, 0);
  add_file(config->bump_files, &config->bump_file_count, "npm-shrinkwrap.json",
           "json", NULL, 0);
}

static int read_string_value(const toml_table_t *table, const char *key,
                             char *target, size_t target_size, char *error,
                             size_t error_size) {
  toml_datum_t value;

  if (!toml_key_exists(table, key))
    return 1;
  value = toml_string_in(table, key);
  if (!value.ok) {
    set_error(error, error_size, "expected a TOML string");
    return 0;
  }
  if (!copy_text(target, target_size, value.u.s, error, error_size)) {
    free(value.u.s);
    return 0;
  }
  free(value.u.s);
  return 1;
}

static int read_bool_value(const toml_table_t *table, const char *key,
                           bool *target, char *error, size_t error_size) {
  toml_datum_t value;

  if (!toml_key_exists(table, key))
    return 1;
  value = toml_bool_in(table, key);
  if (!value.ok) {
    set_error(error, error_size, "expected a TOML boolean");
    return 0;
  }
  *target = value.u.b != 0;
  return 1;
}

static int read_uint_value(const toml_table_t *table, const char *key,
                           unsigned *target, char *error, size_t error_size) {
  toml_datum_t value;

  if (!toml_key_exists(table, key))
    return 1;
  value = toml_int_in(table, key);
  if (!value.ok || value.u.i < 0 || value.u.i > UINT_MAX) {
    set_error(error, error_size, "expected a non-negative TOML integer");
    return 0;
  }
  *target = (unsigned)value.u.i;
  return 1;
}

static int read_file_array(CsemverConfig *config, const toml_table_t *root,
                           const char *key, bool package_files, char *error,
                           size_t error_size) {
  toml_array_t *array = toml_array_in(root, key);
  CsemverFile *files =
      package_files ? config->package_files : config->bump_files;
  size_t *count =
      package_files ? &config->package_file_count : &config->bump_file_count;
  int index;

  if (array == NULL)
    return 1;
  *count = 0;
  if (package_files)
    config->package_files_explicit = true;
  else
    config->bump_files_explicit = true;
  for (index = 0; index < toml_array_nelem(array); ++index) {
    toml_datum_t path = toml_string_at(array, index);
    if (path.ok) {
      if (!add_file(files, count, path.u.s, NULL, error, error_size)) {
        free(path.u.s);
        return 0;
      }
      free(path.u.s);
      continue;
    }
    {
      toml_table_t *item = toml_table_at(array, index);
      toml_datum_t filename;
      toml_datum_t type;
      toml_datum_t pattern;
      toml_datum_t version_group;
      bool has_pattern;
      bool has_version_group;
      if (item == NULL || !toml_key_exists(item, "filename")) {
        set_error(error, error_size, "file entries need a filename string");
        return 0;
      }
      if (toml_key_exists(item, "updater")) {
        set_error(error, error_size,
                  "executable updater programs are not supported; use the "
                  "regex updater");
        return 0;
      }
      filename = toml_string_in(item, "filename");
      type = toml_string_in(item, "type");
      pattern = toml_string_in(item, "pattern");
      version_group = toml_int_in(item, "versionGroup");
      has_pattern = toml_key_exists(item, "pattern");
      has_version_group = toml_key_exists(item, "versionGroup");
      if (has_pattern &&
          (!pattern.ok || !type.ok || strcmp(type.u.s, "regex") != 0)) {
        if (filename.ok)
          free(filename.u.s);
        if (type.ok)
          free(type.u.s);
        if (pattern.ok)
          free(pattern.u.s);
        set_error(
            error, error_size,
            "pattern file entries need type = regex and a pattern string");
        return 0;
      }
      if ((has_version_group &&
           (!version_group.ok || !has_pattern || version_group.u.i < 0 ||
            version_group.u.i >= 64)) ||
          (type.ok && strcmp(type.u.s, "regex") == 0 && !has_pattern)) {
        if (filename.ok)
          free(filename.u.s);
        if (type.ok)
          free(type.u.s);
        if (pattern.ok)
          free(pattern.u.s);
        set_error(error, error_size,
                  "regex file entries need a pattern and valid versionGroup");
        return 0;
      }
      if (!filename.ok ||
          !add_file(files, count, filename.u.s, type.ok ? type.u.s : NULL,
                    error, error_size)) {
        if (filename.ok)
          free(filename.u.s);
        if (type.ok)
          free(type.u.s);
        if (pattern.ok)
          free(pattern.u.s);
        if (error != NULL && error[0] == '\0')
          set_error(error, error_size,
                    "file filename and type must be strings");
        return 0;
      }
      if (has_pattern) {
        CsemverFile *file = &files[*count - 1];
        if (!copy_text(file->version_pattern, sizeof file->version_pattern,
                       pattern.u.s, error, error_size)) {
          free(filename.u.s);
          if (type.ok)
            free(type.u.s);
          free(pattern.u.s);
          return 0;
        }
        file->version_group =
            has_version_group ? (unsigned)version_group.u.i : 1;
        file->has_version_pattern = true;
      }
      free(filename.u.s);
      if (type.ok)
        free(type.u.s);
      if (pattern.ok)
        free(pattern.u.s);
    }
  }
  return 1;
}

static int read_string_array(const toml_table_t *root, const char *key,
                             char values[][64], size_t *count, size_t limit,
                             char *error, size_t error_size) {
  toml_array_t *array = toml_array_in(root, key);
  int index;

  if (array == NULL)
    return 1;
  *count = 0;
  for (index = 0; index < toml_array_nelem(array); ++index) {
    toml_datum_t value = toml_string_at(array, index);
    if (!value.ok || *count >= limit ||
        !copy_text(values[*count], 64, value.u.s, error, error_size)) {
      if (value.ok)
        free(value.u.s);
      if (error != NULL && error[0] == '\0')
        set_error(error, error_size, "expected a bounded string array");
      return 0;
    }
    ++*count;
    free(value.u.s);
  }
  return 1;
}

static int read_type_array(CsemverConfig *config, const toml_table_t *root,
                           char *error, size_t error_size) {
  toml_array_t *array = toml_array_in(root, "types");
  int index;

  if (array == NULL)
    return 1;
  config->commit_type_count = 0;
  for (index = 0; index < toml_array_nelem(array); ++index) {
    toml_table_t *table = toml_table_at(array, index);
    toml_datum_t type;
    toml_datum_t section;
    toml_datum_t hidden;
    toml_datum_t effect;
    CsemverCommitType *entry;

    if (table == NULL || !toml_key_exists(table, "type") ||
        config->commit_type_count >= CSEMVER_MAX_TYPES) {
      set_error(error, error_size, "invalid or excessive commit type list");
      return 0;
    }
    type = toml_string_in(table, "type");
    section = toml_string_in(table, "section");
    hidden = toml_bool_in(table, "hidden");
    effect = toml_string_in(table, "effect");
    if (!type.ok || (toml_key_exists(table, "section") && !section.ok) ||
        (toml_key_exists(table, "hidden") && !hidden.ok) ||
        (toml_key_exists(table, "effect") && !effect.ok)) {
      if (type.ok)
        free(type.u.s);
      if (section.ok)
        free(section.u.s);
      if (effect.ok)
        free(effect.u.s);
      set_error(error, error_size, "invalid commit type fields");
      return 0;
    }
    entry = &config->commit_types[config->commit_type_count++];
    if (!copy_text(entry->type, sizeof entry->type, type.u.s, error,
                   error_size) ||
        (section.ok && !copy_text(entry->section, sizeof entry->section,
                                  section.u.s, error, error_size))) {
      free(type.u.s);
      if (section.ok)
        free(section.u.s);
      if (effect.ok)
        free(effect.u.s);
      return 0;
    }
    entry->hidden = hidden.ok && hidden.u.b;
    entry->bump = !entry->hidden;
    if (effect.ok) {
      if (strcmp(effect.u.s, "hidden") == 0) {
        entry->hidden = true;
        entry->bump = false;
      } else if (strcmp(effect.u.s, "changelog") == 0) {
        entry->hidden = false;
        entry->bump = false;
      } else if (strcmp(effect.u.s, "bump") == 0) {
        entry->hidden = false;
        entry->bump = true;
      } else {
        free(type.u.s);
        if (section.ok)
          free(section.u.s);
        free(effect.u.s);
        set_error(error, error_size, "unknown commit type effect");
        return 0;
      }
    }
    free(type.u.s);
    if (section.ok)
      free(section.u.s);
    if (effect.ok)
      free(effect.u.s);
  }
  return 1;
}

static int read_scripts(CsemverConfig *config, const toml_table_t *root,
                        char *error, size_t error_size) {
  const toml_table_t *table = toml_table_in(root, "scripts");
  size_t index;

  if (table == NULL)
    return 1;
  config->script_count = 0;
  for (index = 0;
       index < sizeof lifecycle_script_names / sizeof lifecycle_script_names[0];
       ++index) {
    toml_datum_t command;
    CsemverScript *script;
    if (!toml_key_exists(table, lifecycle_script_names[index]))
      continue;
    command = toml_string_in(table, lifecycle_script_names[index]);
    if (!command.ok || config->script_count >= CSEMVER_MAX_SCRIPT) {
      if (command.ok)
        free(command.u.s);
      set_error(error, error_size, "lifecycle script must be a string");
      return 0;
    }
    script = &config->scripts[config->script_count++];
    snprintf(script->name, sizeof script->name, "%s",
             lifecycle_script_names[index]);
    if (!copy_text(script->command, sizeof script->command, command.u.s, error,
                   error_size)) {
      free(command.u.s);
      return 0;
    }
    free(command.u.s);
  }
  return 1;
}

static int read_skip(CsemverConfig *config, const toml_table_t *root,
                     char *error, size_t error_size) {
  const toml_table_t *table = toml_table_in(root, "skip");

  if (table == NULL)
    return 1;
  return read_bool_value(table, "bump", &config->skip_bump, error,
                         error_size) &&
         read_bool_value(table, "changelog", &config->skip_changelog, error,
                         error_size) &&
         read_bool_value(table, "commit", &config->skip_commit, error,
                         error_size) &&
         read_bool_value(table, "tag", &config->skip_tag, error, error_size);
}

int csemver_config_parse(CsemverConfig *config, const char *toml, char *error,
                         size_t error_size) {
  char parse_error[256] = {0};
  char *copy;
  toml_table_t *root;
  size_t index;
  bool *flags[] = {
      &config->first_release,    &config->sign,
      &config->signoff,          &config->no_verify,
      &config->commit_all,       &config->silent,
      &config->tag_force,        &config->dry_run,
      &config->git_tag_fallback, &config->no_bump_when_empty_changes,
      &config->pre_major};
  static const char *const flag_names[] = {
      "firstRelease",   "sign",
      "signoff",        "noVerify",
      "commitAll",      "silent",
      "tagForce",       "dryRun",
      "gitTagFallback", "noBumpWhenEmptyChanges",
      "preMajor"};
  char *string_targets[] = {config->tag_prefix,
                            config->infile,
                            config->header,
                            config->release_as,
                            config->release_commit_message_format,
                            config->message,
                            config->path,
                            config->preset,
                            config->lerna_package,
                            config->npm_publish_hint,
                            config->commit_url_format,
                            config->compare_url_format,
                            config->issue_url_format,
                            config->user_url_format,
                            config->prerelease_id};
  const size_t string_sizes[] = {sizeof config->tag_prefix,
                                 sizeof config->infile,
                                 sizeof config->header,
                                 sizeof config->release_as,
                                 sizeof config->release_commit_message_format,
                                 sizeof config->message,
                                 sizeof config->path,
                                 sizeof config->preset,
                                 sizeof config->lerna_package,
                                 sizeof config->npm_publish_hint,
                                 sizeof config->commit_url_format,
                                 sizeof config->compare_url_format,
                                 sizeof config->issue_url_format,
                                 sizeof config->user_url_format,
                                 sizeof config->prerelease_id};
  static const char *const string_names[] = {"tagPrefix",
                                             "infile",
                                             "header",
                                             "releaseAs",
                                             "releaseCommitMessageFormat",
                                             "message",
                                             "path",
                                             "preset",
                                             "lernaPackage",
                                             "npmPublishHint",
                                             "commitUrlFormat",
                                             "compareUrlFormat",
                                             "issueUrlFormat",
                                             "userUrlFormat",
                                             "prerelease"};

  if (error != NULL && error_size > 0)
    error[0] = '\0';
  if (config == NULL || toml == NULL) {
    set_error(error, error_size, "missing TOML configuration");
    return 0;
  }
  copy = strdup(toml);
  if (copy == NULL) {
    set_error(error, error_size, "out of memory reading TOML configuration");
    return 0;
  }
  root = toml_parse(copy, parse_error, sizeof parse_error);
  free(copy);
  if (root == NULL) {
    if (error != NULL && error_size > 0)
      snprintf(error, error_size, "invalid TOML: %s", parse_error);
    return 0;
  }

  for (index = 0; index < sizeof string_names / sizeof string_names[0];
       ++index) {
    if (!read_string_value(root, string_names[index], string_targets[index],
                           string_sizes[index], error, error_size))
      goto fail;
  }
  config->commit_url_format_explicit = toml_key_exists(root, "commitUrlFormat");
  config->compare_url_format_explicit =
      toml_key_exists(root, "compareUrlFormat");
  config->issue_url_format_explicit = toml_key_exists(root, "issueUrlFormat");
  config->user_url_format_explicit = toml_key_exists(root, "userUrlFormat");
  config->has_prerelease = toml_key_exists(root, "prerelease");
  config->has_message = toml_key_exists(root, "message");
  config->has_changelog_header = toml_key_exists(root, "changelogHeader");
  if (config->has_changelog_header &&
      !read_string_value(root, "changelogHeader", config->changelog_header,
                         sizeof config->changelog_header, error, error_size))
    goto fail;
  for (index = 0; index < sizeof flag_names / sizeof flag_names[0]; ++index) {
    if (!read_bool_value(root, flag_names[index], flags[index], error,
                         error_size))
      goto fail;
  }
  if (!read_uint_value(root, "releaseCount", &config->release_count, error,
                       error_size))
    goto fail;

  if (!read_file_array(config, root, "packageFiles", true, error, error_size) ||
      !read_file_array(config, root, "bumpFiles", false, error, error_size) ||
      !read_string_array(root, "issuePrefixes", config->issue_prefixes,
                         &config->issue_prefix_count, CSEMVER_MAX_PREFIXES,
                         error, error_size) ||
      !read_type_array(config, root, error, error_size) ||
      !read_scripts(config, root, error, error_size))
    goto fail;
  if (!read_skip(config, root, error, error_size))
    goto fail;
  toml_free(root);
  return 1;

fail:
  toml_free(root);
  return 0;
}

int csemver_config_set_string(CsemverConfig *config, const char *key,
                              const char *value, char *error,
                              size_t error_size) {
  char *target = NULL;
  size_t target_size = 0;

  if (strcmp(key, "release-as") == 0 || strcmp(key, "releaseAs") == 0)
    target = config->release_as, target_size = sizeof config->release_as;
  else if (strcmp(key, "prerelease") == 0) {
    target = config->prerelease_id;
    target_size = sizeof config->prerelease_id;
    config->has_prerelease = true;
  } else if (strcmp(key, "infile") == 0 || strcmp(key, "i") == 0)
    target = config->infile, target_size = sizeof config->infile;
  else if (strcmp(key, "tag-prefix") == 0 || strcmp(key, "tagPrefix") == 0 ||
           strcmp(key, "t") == 0)
    target = config->tag_prefix, target_size = sizeof config->tag_prefix;
  else if (strcmp(key, "message") == 0 || strcmp(key, "m") == 0) {
    target = config->message;
    target_size = sizeof config->message;
    config->has_message = true;
  } else if (strcmp(key, "header") == 0)
    target = config->header, target_size = sizeof config->header;
  else if (strcmp(key, "changelogHeader") == 0) {
    target = config->changelog_header;
    target_size = sizeof config->changelog_header;
    config->has_changelog_header = true;
  } else if (strcmp(key, "releaseCommitMessageFormat") == 0 ||
             strcmp(key, "release-commit-message-format") == 0)
    target = config->release_commit_message_format,
    target_size = sizeof config->release_commit_message_format;
  else if (strcmp(key, "path") == 0)
    target = config->path, target_size = sizeof config->path;
  else if (strcmp(key, "preset") == 0)
    target = config->preset, target_size = sizeof config->preset;
  else if (strcmp(key, "lerna-package") == 0 ||
           strcmp(key, "lernaPackage") == 0)
    target = config->lerna_package, target_size = sizeof config->lerna_package;
  else if (strcmp(key, "npmPublishHint") == 0 ||
           strcmp(key, "npm-publish-hint") == 0)
    target = config->npm_publish_hint,
    target_size = sizeof config->npm_publish_hint;
  else if (strcmp(key, "commitUrlFormat") == 0 ||
           strcmp(key, "commit-url-format") == 0) {
    target = config->commit_url_format;
    target_size = sizeof config->commit_url_format;
    config->commit_url_format_explicit = true;
  } else if (strcmp(key, "compareUrlFormat") == 0 ||
             strcmp(key, "compare-url-format") == 0) {
    target = config->compare_url_format;
    target_size = sizeof config->compare_url_format;
    config->compare_url_format_explicit = true;
  } else if (strcmp(key, "issueUrlFormat") == 0 ||
             strcmp(key, "issue-url-format") == 0) {
    target = config->issue_url_format;
    target_size = sizeof config->issue_url_format;
    config->issue_url_format_explicit = true;
  } else if (strcmp(key, "userUrlFormat") == 0 ||
             strcmp(key, "user-url-format") == 0) {
    target = config->user_url_format;
    target_size = sizeof config->user_url_format;
    config->user_url_format_explicit = true;
  } else {
    if (error != NULL && error_size > 0)
      snprintf(error, error_size, "unknown string option: %s", key);
    return 0;
  }
  return copy_text(target, target_size, value, error, error_size);
}

int csemver_config_set_script(CsemverConfig *config, const char *name,
                              const char *command, char *error,
                              size_t error_size) {
  CsemverScript *script = NULL;
  size_t index;
  if (name == NULL || command == NULL) {
    set_error(error, error_size,
              "lifecycle script name and command are required");
    return 0;
  }
  for (index = 0;
       index < sizeof lifecycle_script_names / sizeof lifecycle_script_names[0];
       ++index)
    if (strcmp(name, lifecycle_script_names[index]) == 0)
      break;
  if (index == sizeof lifecycle_script_names / sizeof lifecycle_script_names[0])
    return 1;
  for (index = 0; index < config->script_count; ++index)
    if (strcmp(config->scripts[index].name, name) == 0) {
      script = &config->scripts[index];
      break;
    }
  if (script == NULL) {
    if (config->script_count >= CSEMVER_MAX_SCRIPT) {
      set_error(error, error_size, "too many lifecycle scripts");
      return 0;
    }
    script = &config->scripts[config->script_count];
    if (!copy_text(script->name, sizeof script->name, name, error, error_size))
      return 0;
    if (!copy_text(script->command, sizeof script->command, command, error,
                   error_size))
      return 0;
    ++config->script_count;
    return 1;
  }
  return copy_text(script->command, sizeof script->command, command, error,
                   error_size);
}

int csemver_config_set_bool(CsemverConfig *config, const char *key, bool value,
                            char *error, size_t error_size) {
  bool *target = NULL;

  if (strcmp(key, "first-release") == 0 || strcmp(key, "firstRelease") == 0 ||
      strcmp(key, "f") == 0)
    target = &config->first_release;
  else if (strcmp(key, "sign") == 0 || strcmp(key, "s") == 0)
    target = &config->sign;
  else if (strcmp(key, "signoff") == 0)
    target = &config->signoff;
  else if (strcmp(key, "no-verify") == 0 || strcmp(key, "noVerify") == 0 ||
           strcmp(key, "n") == 0)
    target = &config->no_verify;
  else if (strcmp(key, "commit-all") == 0 || strcmp(key, "commitAll") == 0 ||
           strcmp(key, "a") == 0)
    target = &config->commit_all;
  else if (strcmp(key, "silent") == 0)
    target = &config->silent;
  else if (strcmp(key, "tag-force") == 0 || strcmp(key, "tagForce") == 0)
    target = &config->tag_force;
  else if (strcmp(key, "dry-run") == 0 || strcmp(key, "dryRun") == 0)
    target = &config->dry_run;
  else if (strcmp(key, "git-tag-fallback") == 0 ||
           strcmp(key, "gitTagFallback") == 0)
    target = &config->git_tag_fallback;
  else if (strcmp(key, "noBumpWhenEmptyChanges") == 0 ||
           strcmp(key, "no-bump-when-empty-changes") == 0)
    target = &config->no_bump_when_empty_changes;
  else if (strcmp(key, "preMajor") == 0 || strcmp(key, "pre-major") == 0)
    target = &config->pre_major;
  else if (strcmp(key, "skip.bump") == 0)
    target = &config->skip_bump;
  else if (strcmp(key, "skip.changelog") == 0)
    target = &config->skip_changelog;
  else if (strcmp(key, "skip.commit") == 0)
    target = &config->skip_commit;
  else if (strcmp(key, "skip.tag") == 0)
    target = &config->skip_tag;
  else {
    if (error != NULL && error_size > 0)
      snprintf(error, error_size, "unknown boolean option: %s", key);
    return 0;
  }
  *target = value;
  return 1;
}

int csemver_config_set_array(CsemverConfig *config, const char *key,
                             const char *const *values, size_t count,
                             char *error, size_t error_size) {
  size_t index;

  if (strcmp(key, "types") == 0) {
    /* yargs passes these config-spec array entries as strings, not objects. */
    config->commit_type_count = 0;
    return 1;
  }
  if (strcmp(key, "packageFiles") == 0 || strcmp(key, "package-files") == 0) {
    config->package_file_count = 0;
    config->package_files_explicit = true;
    for (index = 0; index < count; ++index) {
      if (!add_file(config->package_files, &config->package_file_count,
                    values[index], NULL, error, error_size))
        return 0;
    }
    return 1;
  }
  if (strcmp(key, "bumpFiles") == 0 || strcmp(key, "bump-files") == 0) {
    config->bump_file_count = 0;
    config->bump_files_explicit = true;
    for (index = 0; index < count; ++index) {
      if (!add_file(config->bump_files, &config->bump_file_count, values[index],
                    NULL, error, error_size))
        return 0;
    }
    return 1;
  }
  if (strcmp(key, "issuePrefixes") == 0 || strcmp(key, "issue-prefixes") == 0) {
    if (count > CSEMVER_MAX_PREFIXES) {
      set_error(error, error_size, "too many issue prefixes");
      return 0;
    }
    config->issue_prefix_count = 0;
    for (index = 0; index < count; ++index) {
      if (!copy_text(config->issue_prefixes[index], 64, values[index], error,
                     error_size))
        return 0;
      ++config->issue_prefix_count;
    }
    return 1;
  }
  if (error != NULL && error_size > 0)
    snprintf(error, error_size, "unknown array option: %s", key);
  return 0;
}
