#include "../src/version.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
  test_json_round_trip();
  test_package_lock_updates_only_root_package();
  test_plain_text_preserves_upstream_write_semantics();
  test_toml_and_yaml_surface();
  puts("version file tests passed");
  return 0;
}
