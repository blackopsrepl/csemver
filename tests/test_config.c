#include "../src/config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  CsemverConfig config;
  char error[256];
  const char *toml =
      "tagPrefix = \"\"\n"
      "infile = \"docs/CHANGELOG.md\"\n"
      "releaseAs = \"minor\"\n"
      "releaseCount = 0\n"
      "firstRelease = true\n"
      "preMajor = true\n"
      "header = \"Modern header\"\n"
      "changelogHeader = \"Legacy header\"\n"
      "packageFiles = [{ filename = \"VERSION\", type = \"plain-text\" }]\n"
      "bumpFiles = [\"VERSION\"]\n"
      "issuePrefixes = [\"#\", \"GH-\"]\n"
      "types = [{ type = \"feature\", section = \"Features\" }, "
      "{ type = \"private\", hidden = true }]\n"
      "[skip]\n"
      "bump = true\n"
      "[scripts]\n"
      "precommit = \"printf lifecycle\"\n";

  csemver_config_defaults(&config);
  assert(strcmp(config.tag_prefix, "v") == 0);
  assert(config.release_count == 1);
  assert(config.package_file_count == 3);
  assert(config.bump_file_count == 5);
  assert(csemver_config_parse(&config, toml, error, sizeof error));
  assert(config.tag_prefix[0] == '\0');
  assert(strcmp(config.infile, "docs/CHANGELOG.md") == 0);
  assert(strcmp(config.release_as, "minor") == 0);
  assert(config.release_count == 0);
  assert(config.first_release && config.pre_major && config.skip_bump);
  assert(strcmp(config.header, "Modern header") == 0);
  assert(strcmp(config.changelog_header, "Legacy header") == 0);
  assert(config.has_changelog_header);
  assert(config.package_files_explicit && config.package_file_count == 1);
  assert(strcmp(config.package_files[0].filename, "VERSION") == 0);
  assert(strcmp(config.package_files[0].type, "plain-text") == 0);
  assert(config.bump_files_explicit && config.bump_file_count == 1);
  assert(strcmp(config.bump_files[0].filename, "VERSION") == 0);
  assert(config.issue_prefix_count == 2);
  assert(strcmp(config.issue_prefixes[1], "GH-") == 0);
  assert(config.commit_type_count == 2);
  assert(strcmp(config.commit_types[0].section, "Features") == 0);
  assert(config.commit_types[1].hidden);
  assert(config.script_count == 1);
  assert(strcmp(config.scripts[0].name, "precommit") == 0);
  assert(strcmp(config.scripts[0].command, "printf lifecycle") == 0);
  assert(csemver_config_set_string(&config, "release-as", "2.0.0", error,
                                   sizeof error));
  assert(strcmp(config.release_as, "2.0.0") == 0);
  assert(
      csemver_config_set_bool(&config, "skip.tag", true, error, sizeof error));
  assert(config.skip_tag);
  {
    const char *files[] = {"VERSION", "pyproject.toml"};
    assert(csemver_config_set_array(&config, "bumpFiles", files, 2, error,
                                    sizeof error));
    assert(config.bump_files_explicit && config.bump_file_count == 2);
  }
  {
    const char *package_files_only = "packageFiles = [\"package.json\"]\n";
    size_t package_bump_entries = 0;
    size_t index;
    csemver_config_defaults(&config);
    assert(
        csemver_config_parse(&config, package_files_only, error, sizeof error));
    assert(config.package_file_count == 1);
    assert(config.bump_file_count == 5);
    for (index = 0; index < config.bump_file_count; ++index)
      if (strcmp(config.bump_files[index].filename, "package.json") == 0)
        ++package_bump_entries;
    assert(package_bump_entries == 1);
  }
  {
    const char *pattern_files =
        "packageFiles = [{ filename = \"lib/release.rb\", type = \"regex\", "
        "pattern = '^(  VERSION = \")([^\"]+)(\")$', versionGroup = 2 }]\n"
        "bumpFiles = [{ filename = \"README.md\", type = \"regex\", "
        "pattern = '^(v)([^ ]+)$' }]\n";
    csemver_config_defaults(&config);
    assert(csemver_config_parse(&config, pattern_files, error, sizeof error));
    assert(config.package_files[0].has_version_pattern);
    assert(strcmp(config.package_files[0].type, "regex") == 0);
    assert(strcmp(config.package_files[0].version_pattern,
                  "^(  VERSION = \")([^\"]+)(\")$") == 0);
    assert(config.package_files[0].version_group == 2);
    assert(config.bump_files[0].has_version_pattern);
    assert(config.bump_files[0].version_group == 1);
    assert(
        !csemver_config_parse(&config,
                              "bumpFiles = [{ filename = \"README.md\", type = "
                              "\"regex\" }]\n",
                              error, sizeof error));
  }
  {
    const char *pattern_package_only =
        "packageFiles = [{ filename = \"src/custom.c\", type = \"regex\", "
        "pattern = '^(VERSION = )([0-9.]+)$', versionGroup = 2 }]\n";
    size_t index;
    bool custom_bump_target_found = false;
    csemver_config_defaults(&config);
    assert(csemver_config_parse(&config, pattern_package_only, error,
                                sizeof error));
    for (index = 0; index < config.bump_file_count; ++index) {
      if (strcmp(config.bump_files[index].filename, "src/custom.c") == 0) {
        custom_bump_target_found = true;
      }
    }
    assert(config.bump_file_count == 5);
    assert(!custom_bump_target_found);
  }
  assert(!csemver_config_parse(&config, "releaseAs = [", error, sizeof error));
  assert(strstr(error, "invalid TOML:") == error);
  puts("configuration tests passed");
  return 0;
}
