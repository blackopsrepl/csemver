#include "../src/version.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_repository_url_forms(void) {
  const char *object_form =
      "{\"name\":\"fixture\",\"repository\":{\"type\":\"git\","
      "\"url\":\"https://github.com/example/project.git\"}}";
  const char *string_form =
      "{\"repository\":\"https://github.com/example/other.git\"}";
  char url[256];

  assert(csemver_json_repository_url(object_form, url, sizeof url));
  assert(strcmp(url, "https://github.com/example/project.git") == 0);
  assert(csemver_json_repository_url(string_form, url, sizeof url));
  assert(strcmp(url, "https://github.com/example/other.git") == 0);
}

static void test_json_package_config_strings(void) {
  const char *package_json =
      "{\"name\":\"fixture\",\"standard-version\":{"
      "\"tagPrefix\":\"legacy-\"},\"commit-and-tag-version\":{"
      "\"tagPrefix\":\"release-\",\"header\":\"Line\\nHeader\"}}";
  char value[128];

  assert(csemver_json_object_string(package_json, "standard-version",
                                    "tagPrefix", value, sizeof value));
  assert(strcmp(value, "legacy-") == 0);
  assert(csemver_json_object_string(package_json, "commit-and-tag-version",
                                    "tagPrefix", value, sizeof value));
  assert(strcmp(value, "release-") == 0);
  assert(csemver_json_object_string(package_json, "commit-and-tag-version",
                                    "header", value, sizeof value));
  assert(strcmp(value, "Line\nHeader") == 0);
  assert(!csemver_json_object_string(package_json, "missing", "tagPrefix",
                                     value, sizeof value));
}

static void test_json_package_config_booleans(void) {
  const char *package_json = "{\"commit-and-tag-version\":{\"dryRun\":true,"
                             "\"silent\":false,\"invalid\":1}}";
  bool value = false;

  assert(csemver_json_object_boolean(package_json, "commit-and-tag-version",
                                     "dryRun", &value));
  assert(value);
  assert(csemver_json_object_boolean(package_json, "commit-and-tag-version",
                                     "silent", &value));
  assert(!value);
  value = true;
  assert(!csemver_json_object_boolean(package_json, "commit-and-tag-version",
                                      "invalid", &value));
  assert(value);
}

static void test_json_package_config_nested_booleans(void) {
  const char *package_json = "{\"commit-and-tag-version\":{\"skip\":{"
                             "\"changelog\":true,\"commit\":false}}}";
  bool value = false;

  assert(csemver_json_object_nested_boolean(
      package_json, "commit-and-tag-version", "skip", "changelog", &value));
  assert(value);
  assert(csemver_json_object_nested_boolean(
      package_json, "commit-and-tag-version", "skip", "commit", &value));
  assert(!value);
  value = true;
  assert(!csemver_json_object_nested_boolean(
      package_json, "commit-and-tag-version", "skip", "missing", &value));
  assert(value);
}

static void test_json_package_config_string_arrays(void) {
  const char *package_json =
      "{\"standard-version\":{\"issuePrefixes\":[\"JIRA-\",\"GH-\","
      "\"hash\\u002d\"]}}";
  const char *not_strings =
      "{\"standard-version\":{\"issuePrefixes\":[\"GH-\",2]}}";
  char values[4][64];
  size_t count = 99;

  assert(csemver_json_object_string_array(
      package_json, "standard-version", "issuePrefixes", &values[0][0],
      sizeof values[0], sizeof values / sizeof values[0], &count));
  assert(count == 3);
  assert(strcmp(values[0], "JIRA-") == 0);
  assert(strcmp(values[1], "GH-") == 0);
  assert(strcmp(values[2], "hash-") == 0);
  assert(!csemver_json_object_string_array(
      not_strings, "standard-version", "issuePrefixes", &values[0][0],
      sizeof values[0], sizeof values / sizeof values[0], &count));
  assert(count == 0);
}

static void test_json_package_config_unsigned(void) {
  const char *package_json = "{\"commit-and-tag-version\":{\"releaseCount\":0,"
                             "\"release-count\":12,\"fraction\":1.5,"
                             "\"tooLarge\":18446744073709551616}}";
  unsigned value = 7;

  assert(csemver_json_object_unsigned(package_json, "commit-and-tag-version",
                                      "releaseCount", &value));
  assert(value == 0);
  assert(csemver_json_object_unsigned(package_json, "commit-and-tag-version",
                                      "release-count", &value));
  assert(value == 12);
  assert(!csemver_json_object_unsigned(package_json, "commit-and-tag-version",
                                       "fraction", &value));
  assert(value == 12);
  assert(!csemver_json_object_unsigned(package_json, "commit-and-tag-version",
                                       "tooLarge", &value));
  assert(value == 12);
}

static void test_json_package_config_commit_types(void) {
  const char *package_json =
      "{\"commit-and-tag-version\":{\"types\":["
      "{\"type\":\"feature\",\"section\":\"Custom Features\","
      "\"hidden\":false},"
      "{\"type\":\"chore\",\"section\":\"Internal\","
      "\"effect\":\"hidden\"},"
      "{\"type\":\"docs\",\"section\":\"Documentation\","
      "\"hidden\":true,\"effect\":\"changelog\"}]}}";
  const char *invalid_effect = "{\"commit-and-tag-version\":{\"types\":["
                               "{\"type\":\"feat\",\"effect\":\"unknown\"}]}}";
  char types[3][64];
  char sections[3][128];
  bool hidden[3];
  bool bump[3];
  size_t count = 99;

  assert(csemver_json_object_commit_type_array(
      package_json, "commit-and-tag-version", "types", &types[0][0],
      sizeof types[0], &sections[0][0], sizeof sections[0], hidden, bump,
      sizeof types / sizeof types[0], &count));
  assert(count == 3);
  assert(strcmp(types[0], "feature") == 0);
  assert(strcmp(sections[0], "Custom Features") == 0);
  assert(!hidden[0] && bump[0]);
  assert(strcmp(types[1], "chore") == 0);
  assert(hidden[1] && !bump[1]);
  assert(strcmp(types[2], "docs") == 0);
  assert(!hidden[2] && !bump[2]);
  assert(!csemver_json_object_commit_type_array(
      invalid_effect, "commit-and-tag-version", "types", &types[0][0],
      sizeof types[0], &sections[0][0], sizeof sections[0], hidden, bump,
      sizeof types / sizeof types[0], &count));
  assert(count == 0);
}

static void test_json_package_config_typed_file_array(void) {
  const char *package_json =
      "{\"commit-and-tag-version\":{\"bumpFiles\":["
      "{\"filename\":\"VERSION\",\"type\":\"plain-text\"},"
      "{\"filename\":\"manifest.json\",\"type\":\"json\"}]}}";
  const char *custom_updater =
      "{\"commit-and-tag-version\":{\"bumpFiles\":["
      "{\"filename\":\"VERSION\",\"type\":\"plain-text\","
      "\"updater\":\"custom.js\"}]}}";
  const char *mixed_package_files =
      "{\"commit-and-tag-version\":{\"packageFiles\":["
      "\"VERSION.txt\",{\"filename\":\"metadata.json\","
      "\"type\":\"json\"}]}}";
  char filenames[2][128];
  char types[2][32];
  size_t count = 0;

  assert(csemver_json_object_typed_file_array(
      package_json, "commit-and-tag-version", "bumpFiles", &filenames[0][0],
      sizeof filenames[0], &types[0][0], sizeof types[0], 2, &count));
  assert(count == 2);
  assert(strcmp(filenames[0], "VERSION") == 0);
  assert(strcmp(types[0], "plain-text") == 0);
  assert(strcmp(filenames[1], "manifest.json") == 0);
  assert(strcmp(types[1], "json") == 0);
  assert(!csemver_json_object_typed_file_array(
      custom_updater, "commit-and-tag-version", "bumpFiles", &filenames[0][0],
      sizeof filenames[0], &types[0][0], sizeof types[0], 2, &count));
  assert(count == 0);
  assert(!csemver_json_object_typed_file_array(
      mixed_package_files, "commit-and-tag-version", "packageFiles",
      &filenames[0][0], sizeof filenames[0], &types[0][0], sizeof types[0], 2,
      &count));
  assert(count == 0);
  assert(csemver_json_object_mixed_file_array(
      mixed_package_files, "commit-and-tag-version", "packageFiles",
      &filenames[0][0], sizeof filenames[0], &types[0][0], sizeof types[0], 2,
      &count));
  assert(count == 2);
  assert(strcmp(filenames[0], "VERSION.txt") == 0);
  assert(types[0][0] == '\0');
  assert(strcmp(filenames[1], "metadata.json") == 0);
  assert(strcmp(types[1], "json") == 0);
}

static void test_json_round_trip(void) {
  const char *input = "{\r\n\t\"name\": \"fixture\",\r\n"
                      "\t\"version\": \"1.2.3\",\r\n"
                      "\t\"private\": true\r\n}\r\n";
  const char *expected = "{\r\n\t\"name\": \"fixture\",\r\n"
                         "\t\"version\": \"1.3.0\",\r\n"
                         "\t\"private\": true\r\n}\r\n";
  char version[128];
  char error[256];
  bool is_private = false;
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("package.json", "json", input, version,
                                   sizeof version, &is_private, error,
                                   sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(is_private);
  assert(csemver_version_update_text("package.json", "json", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_json_compact_input_uses_default_upstream_indent(void) {
  const char *input = "{\"name\":\"fixture\",\"version\":\"1.2.3\","
                      "\"repository\":{\"type\":\"git\","
                      "\"url\":\"https://github.com/example/project.git\"}}\n";
  const char *expected =
      "{\n  \"name\": \"fixture\",\n"
      "  \"version\": \"1.3.0\",\n"
      "  \"repository\": {\n"
      "    \"type\": \"git\",\n"
      "    \"url\": \"https://github.com/example/project.git\"\n"
      "  }\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("package.json", "json", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_json_escape_sequences_normalize_like_upstream(void) {
  const char *input = "{\"name\":\"\\u0066ixture\\/x\",\"version\":\"1.2.3\"}";
  const char *expected = "{\n  \"name\": \"fixture/x\",\n"
                         "  \"version\": \"1.3.0\"\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("package.json", "json", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_json_number_spelling_normalizes_like_upstream(void) {
  const char *input = "{\"name\":\"fixture\",\"version\":\"1.2.3\","
                      "\"weight\":1.0}";
  const char *expected = "{\n  \"name\": \"fixture\",\n"
                         "  \"version\": \"1.3.0\",\n"
                         "  \"weight\": 1\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("package.json", "json", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_json_integer_keys_reorder_like_upstream(void) {
  const char *input = "{\"name\":\"fixture\",\"version\":\"1.2.3\","
                      "\"10\":\"ten\",\"2\":\"two\"}";
  const char *expected = "{\n  \"2\": \"two\",\n"
                         "  \"10\": \"ten\",\n"
                         "  \"name\": \"fixture\",\n"
                         "  \"version\": \"1.3.0\"\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("package.json", "json", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_json_duplicate_version_uses_last_value(void) {
  const char *input = "{\"version\":\"0.1.0\",\"version\":\"1.2.3\"}";
  const char *expected = "{\n  \"version\": \"1.3.0\"\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("package.json", "json", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(csemver_version_update_text("package.json", "json", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_json_unicode_escapes_in_version_key_and_value(void) {
  const char *input = "{\"\\u0076ersion\":\"\\u0031.2.3\"}";
  const char *expected = "{\n  \"version\": \"1.3.0\"\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("package.json", "json", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(csemver_version_update_text("package.json", "json", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_json_key_with_embedded_nul_does_not_match_version(void) {
  const char *input = "{\"version\\u0000suffix\":\"1.2.3\"}";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
}

static void test_json_version_with_embedded_nul_is_rejected(void) {
  const char *input = "{\"version\":\"1.2.3\\u0000suffix\"}";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
}

static void test_truncated_package_lock_reports_parse_error(void) {
  const char *input = "{\n";
  const char *expected =
      "Expected property name or '}' in JSON at position 2 (line 2 column 1)";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package-lock.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
  assert(strcmp(error, expected) == 0);
}

static void test_package_lock_rejects_non_json_values(void) {
  const char *input = "{\"name\": undefined}";
  const char *expected = "Unexpected token 'u', \"{"
                         "\"name\": undefined}\" is not valid JSON";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package-lock.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
  assert(strcmp(error, expected) == 0);
}

static void test_json_invalid_identifier_diagnostics_match_node(void) {
  const struct {
    const char *input;
    const char *expected;
  } cases[] = {
      {"{\"name\":NaN}",
       "Unexpected token 'N', \"{\"name\":NaN}\" is not valid JSON"},
      {"{\"name\":foo}",
       "Unexpected token 'o', \"{\"name\":foo}\" is not valid JSON"},
      {"{\"version\":\"1.0.0\",\"value\":NaN}",
       "Unexpected token 'N', ...\"\",\"value\":NaN}\" is not valid JSON"},
  };
  char version[128];
  char error[256];
  size_t i;

  for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    assert(!csemver_version_read_text("package-lock.json", "json",
                                      cases[i].input, version, sizeof version,
                                      NULL, error, sizeof error));
    assert(strcmp(error, cases[i].expected) == 0);
  }
}

static void test_json_trailing_comma_diagnostic_matches_node(void) {
  const char *input = "{\"name\":\"x\",}";
  const char *expected = "Expected double-quoted property name in JSON at "
                         "position 12 (line 1 column 13)";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package-lock.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
  assert(strcmp(error, expected) == 0);
}

static void test_json_array_trailing_comma_diagnostic_matches_node(void) {
  const char *input = "{\"name\":[1,]}";
  const char *expected =
      "Unexpected token ']', \"{\"name\":[1,]}\" is not valid JSON";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package-lock.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
  assert(strcmp(error, expected) == 0);
}

static void test_json_missing_object_value_diagnostic_matches_node(void) {
  const char *input = "{\"name\":\"malformed-primary\",\"version\":}\n";
  const char *expected =
      "Unexpected token '}', ...\"\"version\":}\n\" is not valid JSON";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
  assert(strcmp(error, expected) == 0);
}

static void test_json_leading_zero_diagnostic_matches_node(void) {
  const char *input = "{\"name\":01}";
  const char *expected =
      "Unexpected number in JSON at position 9 (line 1 column 10)";
  char version[128];
  char error[256];

  assert(!csemver_version_read_text("package-lock.json", "json", input, version,
                                    sizeof version, NULL, error, sizeof error));
  assert(strcmp(error, expected) == 0);
}

static void test_package_lock_updates_only_root_package(void) {
  const char *input = "{\n  \"version\": \"1.0.0\",\n"
                      "  \"packages\": {\n    \"\": {\n"
                      "      \"version\": \"1.0.0\"\n    },\n"
                      "    \"node_modules/a\": {\n"
                      "      \"version\": \"9.9.9\"\n    }\n  }\n}\n";
  const char *expected = "{\n  \"version\": \"2.0.0\",\n"
                         "  \"packages\": {\n    \"\": {\n"
                         "      \"version\": \"2.0.0\"\n    },\n"
                         "    \"node_modules/a\": {\n"
                         "      \"version\": \"9.9.9\"\n    }\n  }\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  if (!csemver_version_update_text("package-lock.json", "json", input, "2.0.0",
                                   &updated, &updated_size, version,
                                   sizeof version, error, sizeof error)) {
    fprintf(stderr, "version update failed: %s\n", error);
    assert(0);
  }
  assert(strcmp(version, "1.0.0") == 0);
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_package_lock_adds_missing_root_package_version(void) {
  const char *input =
      "{\"version\":\"1.2.3\",\"packages\":{\"\":{\"name\":\"fixture\"}}}";
  const char *expected = "{\n  \"version\": \"1.3.0\",\n"
                         "  \"packages\": {\n    \"\": {\n"
                         "      \"name\": \"fixture\",\n"
                         "      \"version\": \"1.3.0\"\n"
                         "    }\n  }\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("package-lock.json", "json", input,
                                     "1.3.0", &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_package_lock_adds_missing_root_version_fields(void) {
  const char *input = "{\n  \"name\": \"missing-root-lock-version\",\n"
                      "  \"lockfileVersion\": 3,\n  \"requires\": true,\n"
                      "  \"packages\": {\n    \"\": {\n"
                      "      \"name\": \"missing-root-lock-version\"\n"
                      "    }\n  }\n}\n";
  const char *expected = "{\n  \"name\": \"missing-root-lock-version\",\n"
                         "  \"lockfileVersion\": 3,\n  \"requires\": true,\n"
                         "  \"packages\": {\n    \"\": {\n"
                         "      \"name\": \"missing-root-lock-version\",\n"
                         "      \"version\": \"1.1.0\"\n    }\n  },\n"
                         "  \"version\": \"1.1.0\"\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("package-lock.json", "json", input,
                                     "1.1.0", &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(strcmp(version, "undefined") == 0);
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_plain_text_preserves_upstream_write_semantics(void) {
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("VERSION.txt", "plain-text", "1.0.0\n",
                                   version, sizeof version, NULL, error,
                                   sizeof error));
  assert(strcmp(version, "1.0.0") == 0);
  assert(csemver_version_update_text("VERSION.txt", "plain-text", "1.0.0\n",
                                     "1.0.1", &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == 5 && memcmp(updated, "1.0.1", 5) == 0);
  free(updated);
}

static void test_toml_and_yaml_surface(void) {
  const char *toml = "[project]\r\nversion = \"0.4.1\"\r\n";
  const char *yaml = "name: fixture\nversion: 0.4.1\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("pyproject.toml", "python", toml, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "0.4.1") == 0);
  assert(csemver_version_read_text("pubspec.yaml", "yaml", yaml, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "0.4.1") == 0);
  assert(csemver_version_update_text("pubspec.yaml", "yaml", yaml, "0.5.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(strstr(updated, "version: 0.5.0") != NULL);
  free(updated);
}

static void test_yaml_updater_only_updates_root_version(void) {
  const char *input = "nested:\n  version: 0.9.1\nversion: 1.2.3\n";
  const char *expected = "nested:\n  version: 0.9.1\nversion: 1.3.0\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("config.yaml", "yaml", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_openapi_uses_info_version_not_nested_schema_version(void) {
  const char *input =
      "openapi: 3.0.3\ncomponents:\n  schemas:\n    Widget:\n"
      "      version: 0.9.1\ninfo:\n  title: café\n  version: \"1.2.3\"\n"
      "paths: {}\n";
  const char *expected =
      "openapi: 3.0.3\ncomponents:\n  schemas:\n    Widget:\n"
      "      version: 0.9.1\ninfo:\n  title: café\n  version: \"1.3.0\"\n"
      "paths: {}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("openapi.yaml", "openapi", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(csemver_version_update_text("openapi.yaml", "openapi", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_openapi_flow_maps_match_upstream_spacing(void) {
  const char *input = "{openapi: 3.0.3, info: {version: 1.2.3}}\n";
  const char *expected = "{ openapi: 3.0.3, info: { version: 1.3.0 } }\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("openapi.yaml", "openapi", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(csemver_version_update_text("openapi.yaml", "openapi", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_openapi_multiline_flow_comment_matches_upstream(void) {
  const char *input =
      "{openapi: 3.0.3, info: {version: 1.2.3}, # cmt\npaths: {}}\n";
  const char *expected =
      "{\n  openapi: 3.0.3,\n  info: { version: 1.3.0 }, # cmt\n"
      "  paths: {}\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("openapi.yaml", "openapi", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "1.2.3") == 0);
  assert(csemver_version_update_text("openapi.yaml", "openapi", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_yaml_multiline_flow_map_without_comments_collapses(void) {
  const char *input = "{version: 1.2.3,\nother: x}\n";
  const char *expected = "{ version: 1.3.0, other: x }\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void
test_yaml_multiline_flow_comment_before_close_matches_upstream(void) {
  const char *input = "{version: 1.2.3, # cmt\n}\n";
  const char *expected = "{ version: 1.3.0 } # cmt\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_yaml_multiline_flow_comment_on_own_line(void) {
  const char *input = "{version: 1.2.3,\n  # cmt\n  other: x}\n";
  const char *expected = "{\n  version: 1.3.0,\n  # cmt\n  other: x\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_yaml_multiline_flow_leading_comment(void) {
  const char *input = "{ # lead\nversion: 1.2.3, other: x}\n";
  const char *expected = "{\n  # lead\n  version: 1.3.0,\n  other: x\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_yaml_multiline_flow_consecutive_comments(void) {
  const char *input = "{version: 1.2.3, # cmt1\n# cmt2\nother: x}\n";
  const char *expected =
      "{\n  version: 1.3.0, # cmt1\n  # cmt2\n  other: x\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void
test_yaml_multiline_flow_consecutive_comments_with_blank_line(void) {
  const char *input = "{version: 1.2.3, # cmt1\n\n  # cmt2\nother: x}\n";
  const char *expected =
      "{\n  version: 1.3.0, # cmt1\n  # cmt2\n  other: x\n}\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_yaml_no_newline_matches_upstream_output(void) {
  const char *input = "version: 1.2.3";
  const char *expected = "version: 1.3.0undefined";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_yaml_mixed_newlines_match_upstream(void) {
  const char *input = "a: b\r\nversion: 1.2.3\n";
  const char *expected = "a: b\nversion: 1.3.0\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_update_text("config.yaml", "yaml", input, "1.3.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_gradle_updater_matches_upstream(void) {
  const char *input =
      "plugins { }\n\nversion='6.3.1'\njava.sourceCompatibility = 8\n";
  const char *expected =
      "plugins { }\n\nversion = \"6.4.0\"\njava.sourceCompatibility = 8\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("build.gradle.kts", "gradle", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "6.3.1") == 0);
  assert(csemver_version_update_text("build.gradle.kts", "gradle", input,
                                     "6.4.0", &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_gradle_updater_handles_carriage_return_lines(void) {
  const char *input =
      "plugins { }\rversion='6.3.1'\rjava.sourceCompatibility = 8\r";
  const char *expected =
      "plugins { }\rversion = \"6.4.0\"\rjava.sourceCompatibility = 8\r";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("build.gradle", "gradle", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "6.3.1") == 0);
  assert(csemver_version_update_text("build.gradle", "gradle", input, "6.4.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_csproj_updater_matches_upstream(void) {
  const char *input =
      "<Project>\n    <PropertyGroup>\n        <Version>6.3.1</Version>\n"
      "    </PropertyGroup>\n</Project>\n";
  const char *expected =
      "<Project>\n    <PropertyGroup>\n        <Version>6.4.0</Version>\n"
      "    </PropertyGroup>\n</Project>\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("Project.csproj", "csproj", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "6.3.1") == 0);
  assert(csemver_version_update_text("Project.csproj", "csproj", input, "6.4.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_maven_updater_matches_upstream(void) {
  const char *input = "<project>\n  <version>6.3.1</version>\n</project>";
  const char *expected =
      "<project>\n  <version>6.4.0</version>\n</project>\n\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("pom.xml", "maven", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "6.3.1") == 0);
  assert(csemver_version_update_text("pom.xml", "maven", input, "6.4.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
}

static void test_maven_property_and_invalid_shapes(void) {
  const char *property_input =
      "<project>\n  <version>${revision}</version>\n"
      "  <properties>\n    <revision>6.3.1</revision>\n  </properties>\n"
      "</project>";
  const char *property_expected =
      "<project>\n  <version>${revision}</version>\n"
      "  <properties>\n    <revision>6.4.0</revision>\n  </properties>\n"
      "</project>\n\n";
  const char *truncated_input = "<project><version>6.3.1</version>";
  const char *truncated_expected =
      "<project>\n  <version>6.4.0</version>\n</project>\n\n";
  const char *invalid_reference =
      "<project><version>${revision</version></project>";
  const char *missing_property =
      "<project><version>${revision}</version></project>";
  const char *attributed_version =
      "<project><version source=\"x\">6.3.1</version></project>";
  const char *duplicate_version =
      "<project><version>6.3.1</version><version>6.3.2</version></project>";
  const char *prefixed_project =
      "<m:project xmlns:m=\"urn:test\"><m:version>6.3.1</m:version>"
      "</m:project>";
  const char *empty_version = "<project><version/></project>";
  const char *empty_expected =
      "<project>\n  <version>6.4.0</version>\n</project>\n\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("pom.xml", NULL, property_input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "6.3.1") == 0);
  assert(csemver_version_update_text("pom.xml", NULL, property_input, "6.4.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(property_expected));
  assert(memcmp(updated, property_expected, updated_size) == 0);
  free(updated);
  updated = NULL;
  assert(csemver_version_update_text("pom.xml", "maven", truncated_input,
                                     "6.4.0", &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(truncated_expected));
  assert(memcmp(updated, truncated_expected, updated_size) == 0);
  free(updated);
  assert(!csemver_version_read_text("pom.xml", "maven", invalid_reference,
                                    version, sizeof version, NULL, error,
                                    sizeof error));
  assert(strcmp(error, "Failed to read the version field in your pom file - "
                       "unexpected invalid property reference") == 0);
  assert(!csemver_version_read_text("pom.xml", "maven", missing_property,
                                    version, sizeof version, NULL, error,
                                    sizeof error));
  assert(strcmp(error, "Failed to read the revision field in your pom file "
                       "properties - is it present?") == 0);
  assert(!csemver_version_read_text("pom.xml", "maven", attributed_version,
                                    version, sizeof version, NULL, error,
                                    sizeof error));
  assert(strcmp(error, "pomVersion.startsWith is not a function") == 0);
  assert(!csemver_version_read_text("pom.xml", "maven", duplicate_version,
                                    version, sizeof version, NULL, error,
                                    sizeof error));
  assert(strcmp(error, "pomVersion.startsWith is not a function") == 0);
  assert(!csemver_version_read_text("pom.xml", "maven", prefixed_project,
                                    version, sizeof version, NULL, error,
                                    sizeof error));
  assert(strcmp(error,
                "Failed to read the version field in your pom file - is it "
                "present?") == 0);
  assert(!csemver_version_update_text("pom.xml", "maven", prefixed_project,
                                      "6.4.0", &updated, &updated_size, version,
                                      sizeof version, error, sizeof error));
  assert(strcmp(error,
                "Cannot read properties of undefined (reading 'version')") ==
         0);
  assert(csemver_version_update_text("pom.xml", "maven", empty_version, "6.4.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(empty_expected));
  assert(memcmp(updated, empty_expected, updated_size) == 0);
  free(updated);
}

static void test_python_updater_preserves_upstream_first_match(void) {
  const char *input = "# VERSION = '6.3.1'\nversion = \"6.3.1\"\n";
  const char *expected = "# VERSION = '6.4.0'\nversion = \"6.3.1\"\n";
  const char *unmatched = "version =\t\"6.3.1\"\n";
  char version[128];
  char error[256];
  char *updated = NULL;
  size_t updated_size = 0;

  assert(csemver_version_read_text("pyproject.toml", "python", input, version,
                                   sizeof version, NULL, error, sizeof error));
  assert(strcmp(version, "6.3.1") == 0);
  assert(csemver_version_update_text("pyproject.toml", "python", input, "6.4.0",
                                     &updated, &updated_size, version,
                                     sizeof version, error, sizeof error));
  assert(updated_size == strlen(expected));
  assert(memcmp(updated, expected, updated_size) == 0);
  free(updated);
  assert(!csemver_version_update_text("pyproject.toml", "python", unmatched,
                                      "6.4.0", &updated, &updated_size, version,
                                      sizeof version, error, sizeof error));
  assert(strcmp(error,
                "Cannot read properties of undefined (reading 'replace')") ==
         0);
}

int main(void) {
  test_repository_url_forms();
  test_json_package_config_strings();
  test_json_package_config_booleans();
  test_json_package_config_nested_booleans();
  test_json_package_config_string_arrays();
  test_json_package_config_unsigned();
  test_json_package_config_commit_types();
  test_json_package_config_typed_file_array();
  test_json_round_trip();
  test_json_compact_input_uses_default_upstream_indent();
  test_json_escape_sequences_normalize_like_upstream();
  test_json_number_spelling_normalizes_like_upstream();
  test_json_integer_keys_reorder_like_upstream();
  test_json_duplicate_version_uses_last_value();
  test_json_unicode_escapes_in_version_key_and_value();
  test_json_key_with_embedded_nul_does_not_match_version();
  test_json_version_with_embedded_nul_is_rejected();
  test_truncated_package_lock_reports_parse_error();
  test_package_lock_rejects_non_json_values();
  test_json_invalid_identifier_diagnostics_match_node();
  test_json_trailing_comma_diagnostic_matches_node();
  test_json_array_trailing_comma_diagnostic_matches_node();
  test_json_missing_object_value_diagnostic_matches_node();
  test_json_leading_zero_diagnostic_matches_node();
  test_package_lock_updates_only_root_package();
  test_package_lock_adds_missing_root_package_version();
  test_package_lock_adds_missing_root_version_fields();
  test_plain_text_preserves_upstream_write_semantics();
  test_toml_and_yaml_surface();
  test_openapi_uses_info_version_not_nested_schema_version();
  test_openapi_flow_maps_match_upstream_spacing();
  test_openapi_multiline_flow_comment_matches_upstream();
  test_yaml_multiline_flow_map_without_comments_collapses();
  test_yaml_multiline_flow_comment_before_close_matches_upstream();
  test_yaml_multiline_flow_comment_on_own_line();
  test_yaml_multiline_flow_leading_comment();
  test_yaml_multiline_flow_consecutive_comments();
  test_yaml_multiline_flow_consecutive_comments_with_blank_line();
  test_yaml_no_newline_matches_upstream_output();
  test_yaml_mixed_newlines_match_upstream();
  test_yaml_updater_only_updates_root_version();
  test_gradle_updater_matches_upstream();
  test_gradle_updater_handles_carriage_return_lines();
  test_csproj_updater_matches_upstream();
  test_maven_updater_matches_upstream();
  test_maven_property_and_invalid_shapes();
  test_python_updater_preserves_upstream_first_match();
  puts("version file tests passed");
  return 0;
}
